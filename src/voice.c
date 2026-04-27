// ---------------------------------------------------------------------------
//  Voice playback — ElevenLabs PCM → ES8311 codec via the BOX-3 BSP.
//
//  Pipeline:
//    voice_speak("...") → audio_task wakes → ElevenLabs HTTPS POST returns
//    raw 16 kHz / 16-bit / mono PCM → bytes stream into esp_codec_dev_write
//    on the speaker handle returned by bsp_audio_codec_speaker_init().
//
//  Hand-rolled I2S + codec init lived here previously and could not be
//  coaxed into producing real audio on the mic (RX) side. Switched to the
//  BSP so we inherit the chatgpt_demo's known-good ES8311 + ES7210 init.
//  voice.c keeps the ElevenLabs streaming logic + audio task; the codec
//  bring-up is one BSP call. mic.c uses bsp_audio_codec_microphone_init
//  in turn, with both codecs sharing the BSP-managed I2S port.
//
//  Public API: voice_begin / voice_speak / voice_stop / voice_is_speaking
//  / voice_set_volume.
// ---------------------------------------------------------------------------
#include "voice.h"
#include "secrets.h"
#include "wifi_sta.h"
#include "devcfg.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include "esp_codec_dev.h"
#include "bsp/esp-box-3.h"

#include "cJSON.h"

static const char *TAG = "voice";

// 16 kHz mono is the lowest common rate that works for both ES8311 (TTS
// out) and ES7210 (mic in). The BOX-3's I2S port has shared BCLK/LRCK,
// so both directions must run at the same effective sample rate.
// Whisper-class STT also expects 16 kHz, so this is a no-loss choice for
// voice input. ElevenLabs returns pcm_16000 cleanly.
// 16000 Hz matches mic capture rate (BSP audio example uses 16k mono).
#define TTS_SAMPLE_HZ  16000
#define BEEP_SAMPLE_HZ 16000
#define VOICE_TEXT_MAX 512
#define CODEC_WRITE_FRAMES 512

// --- module state ----------------------------------------------------------

static esp_codec_dev_handle_t  s_codec      = NULL;
static TaskHandle_t            s_audio_task = NULL;
static SemaphoreHandle_t       s_request_sem = NULL;

static char     s_pending_text[VOICE_TEXT_MAX] = "";
static volatile bool s_pending_chirp   = false;
static volatile bool s_playing         = false;
static volatile bool s_stop_requested  = false;
static volatile uint8_t s_volume_0_21  = 0;

// --- helpers ---------------------------------------------------------------

static bool has_eleven_key(void) {
    const char *k = ELEVENLABS_API_KEY;
    return k && strlen(k) >= 10 && strncmp(k, "your-", 5) != 0;
}

// 0..21 -> 0..100. Linear taper so the bottom half of the slider feels
// responsive on the small tabletop speaker.
static int volume_percent(void) {
    uint8_t v = s_volume_0_21;
    if (v > 21) v = 21;
    return (int)((v * 100) / 21);
}

static esp_err_t codec_open_for_speak(void) {
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = TTS_SAMPLE_HZ,
        .channel         = 1,
        .bits_per_sample = 16,
    };
    esp_err_t err = esp_codec_dev_open(s_codec, &fs);
    if (err != ESP_OK) return err;
    return esp_codec_dev_set_out_vol(s_codec, volume_percent());
}

// --- boot beep probe -------------------------------------------------------

static void beep_tone(uint16_t freq, uint16_t duration_ms, int16_t amp) {
    if (!s_codec) return;
    const int total = (BEEP_SAMPLE_HZ * duration_ms) / 1000;
    if (total <= 0) return;
    const float phase_inc = 2.0f * (float)M_PI * (float)freq / (float)BEEP_SAMPLE_HZ;
    float phase = 0.0f;
    const int attack  = total / 6;
    const int release = total / 3;

    static int16_t buf[CODEC_WRITE_FRAMES];
    int idx = 0;
    for (int s = 0; s < total; s++) {
        float env = 1.0f;
        if (s < attack)               env = (float)s / (float)attack;
        else if (s > total - release) env = (float)(total - s) / (float)release;
        int16_t v = (int16_t)(sinf(phase) * (float)amp * env);
        phase += phase_inc;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        buf[idx++] = v;
        if (idx >= CODEC_WRITE_FRAMES) {
            esp_codec_dev_write(s_codec, buf, idx * sizeof(int16_t));
            idx = 0;
        }
    }
    if (idx > 0) {
        esp_codec_dev_write(s_codec, buf, idx * sizeof(int16_t));
    }
}

// --- ElevenLabs streaming --------------------------------------------------

typedef struct {
    int16_t  scratch[CODEC_WRITE_FRAMES];
    uint8_t  half_byte;
    bool     has_half;
    size_t   total_samples_written;
    int      http_status;
} tts_ctx_t;

static esp_err_t tts_http_event(esp_http_client_event_t *evt) {
    tts_ctx_t *ctx = (tts_ctx_t *)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key && evt->header_value &&
                (strcasecmp(evt->header_key, "content-type") == 0)) {
                ESP_LOGI(TAG, "content-type: %s", evt->header_value);
            }
            break;

        case HTTP_EVENT_ON_DATA: {
            if (s_stop_requested) {
                ESP_LOGI(TAG, "stop requested; aborting stream");
                return ESP_FAIL;
            }
            if (ctx->http_status >= 400) {
                ESP_LOGW(TAG, "err body (%d): %.*s", ctx->http_status,
                         evt->data_len > 128 ? 128 : evt->data_len,
                         (const char *)evt->data);
                break;
            }

            const uint8_t *raw = (const uint8_t *)evt->data;
            size_t         len = (size_t)evt->data_len;

            if (ctx->has_half && len >= 1) {
                int16_t stitched = (int16_t)((ctx->half_byte) |
                                             ((uint16_t)raw[0] << 8));
                esp_codec_dev_write(s_codec, &stitched, sizeof(stitched));
                ctx->total_samples_written += 1;
                raw += 1;
                len -= 1;
                ctx->has_half = false;
            }
            if (len & 1) {
                ctx->half_byte = raw[len - 1];
                ctx->has_half  = true;
                len -= 1;
            }

            const int16_t *mono = (const int16_t *)raw;
            size_t frames = len / 2;
            while (frames > 0 && !s_stop_requested) {
                size_t this_chunk = frames > CODEC_WRITE_FRAMES
                                  ? CODEC_WRITE_FRAMES : frames;
                memcpy(ctx->scratch, mono, this_chunk * sizeof(int16_t));
                esp_err_t werr = esp_codec_dev_write(s_codec, ctx->scratch,
                                                     this_chunk * sizeof(int16_t));
                if (werr != ESP_OK) {
                    ESP_LOGW(TAG, "codec write err: %s", esp_err_to_name(werr));
                    return ESP_FAIL;
                }
                mono   += this_chunk;
                frames -= this_chunk;
                ctx->total_samples_written += this_chunk;
            }
            break;
        }

        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "stream done: %u mono samples",
                     (unsigned)ctx->total_samples_written);
            break;

        default:
            break;
    }
    return ESP_OK;
}

static bool tts_perform(const char *text) {
    if (!has_eleven_key()) {
        ESP_LOGW(TAG, "no ElevenLabs key configured");
        return false;
    }
    if (!wifi_sta_is_connected()) {
        ESP_LOGW(TAG, "no wifi");
        return false;
    }
    if (!text || !text[0]) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "model_id", ELEVENLABS_MODEL);
    cJSON *vs = cJSON_AddObjectToObject(root, "voice_settings");
    cJSON_AddNumberToObject(vs, "stability",         0.45);
    cJSON_AddNumberToObject(vs, "similarity_boost",  0.8);
    cJSON_AddNumberToObject(vs, "style",             0.3);
    cJSON_AddBoolToObject  (vs, "use_speaker_boost", true);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;

    const char *voice_id = devcfg_voice_id();
    if (!voice_id || !voice_id[0]) voice_id = ELEVENLABS_VOICE_ID;
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.elevenlabs.io/v1/text-to-speech/%s/stream"
             "?output_format=pcm_16000&optimize_streaming_latency=3",
             voice_id);

    tts_ctx_t ctx = {0};
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 25000,
        .event_handler     = tts_http_event,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { free(body); return false; }

    esp_http_client_set_header(h, "xi-api-key",   ELEVENLABS_API_KEY);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Accept",       "audio/pcm");
    esp_http_client_set_post_field(h, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    ctx.http_status = status;
    esp_http_client_cleanup(h);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "perform err: %s (status=%d)", esp_err_to_name(err), status);
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "ElevenLabs HTTP %d", status);
        return false;
    }
    return true;
}

// --- audio task ------------------------------------------------------------

static void audio_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_request_sem, portMAX_DELAY);

        // Chirp first if requested. ~80 ms soft tone — fits between two
        // semaphore wakeups and never blocks a real TTS request because
        // voice_chirp() declines to enqueue while s_playing is true.
        if (s_pending_chirp) {
            s_pending_chirp = false;
            if (codec_open_for_speak() == ESP_OK) {
                beep_tone(520, 80, 5000);
                esp_codec_dev_close(s_codec);
            }
        }

        char local[VOICE_TEXT_MAX];
        strncpy(local, s_pending_text, sizeof(local) - 1);
        local[sizeof(local) - 1] = 0;
        s_pending_text[0] = 0;

        if (!local[0]) continue;

        s_stop_requested = false;
        s_playing = true;
        if (codec_open_for_speak() == ESP_OK) {
            (void)tts_perform(local);
            esp_codec_dev_close(s_codec);
        }
        s_playing = false;
        s_stop_requested = false;
    }
}

// --- public API ------------------------------------------------------------

bool voice_begin(void) {
    if (s_codec) return true;

    s_volume_0_21 = devcfg_volume();

    // BSP brings up I2S TX+RX and ES8311 codec_dev (OUT). Idempotent on
    // I2C bus init — shares with bus.c via bsp_i2c_get_handle().
    s_codec = bsp_audio_codec_speaker_init();
    if (!s_codec) {
        ESP_LOGE(TAG, "bsp_audio_codec_speaker_init failed");
        return false;
    }

    if (codec_open_for_speak() == ESP_OK) {
        ESP_LOGI(TAG, "beep probe (you should hear 3 tones)");
        beep_tone(660,  90, 12000);
        beep_tone(990,  90, 12000);
        beep_tone(1320, 140, 12000);
        esp_codec_dev_close(s_codec);
    }

    s_request_sem = xSemaphoreCreateBinary();
    if (!s_request_sem) {
        ESP_LOGE(TAG, "semaphore create failed");
        return false;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(
        audio_task, "voice_audio", 6144, NULL, 6, &s_audio_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "audio task create failed");
        vSemaphoreDelete(s_request_sem);
        s_request_sem = NULL;
        return false;
    }
    ESP_LOGI(TAG, "voice up @ %d Hz mono via BSP/ES8311", TTS_SAMPLE_HZ);
    return true;
}

bool voice_speak(const char *text) {
    if (!s_codec || !s_request_sem) return false;
    if (!text || !text[0]) return false;
    s_stop_requested = true;
    strncpy(s_pending_text, text, sizeof(s_pending_text) - 1);
    s_pending_text[sizeof(s_pending_text) - 1] = 0;
    xSemaphoreGive(s_request_sem);
    return true;
}

void voice_chirp(void) {
    // Skip if voice isn't up yet, or if TTS is in flight (chirp would queue
    // behind it and play minutes too late). The whole point is immediate
    // feedback when the user releases the talk button.
    if (!s_codec || !s_request_sem || s_playing) return;
    s_pending_chirp = true;
    xSemaphoreGive(s_request_sem);
}

bool voice_is_speaking(void) {
    return s_playing;
}

void voice_stop(void) {
    s_stop_requested = true;
}

void voice_set_volume(uint8_t v) {
    if (v > 21) v = 21;
    s_volume_0_21 = v;
    if (s_codec && s_playing) {
        esp_codec_dev_set_out_vol(s_codec, volume_percent());
    }
}

