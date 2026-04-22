// ---------------------------------------------------------------------------
//  Voice playback implementation — ElevenLabs PCM → I2S_STD → PCM5101.
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

#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "cJSON.h"

static const char *TAG = "voice";

// --- Pinout + audio format -------------------------------------------------
#define I2S_BCLK_PIN   GPIO_NUM_48
#define I2S_LRC_PIN    GPIO_NUM_38
#define I2S_DOUT_PIN   GPIO_NUM_47

#define TTS_SAMPLE_HZ  22050
#define BEEP_SAMPLE_HZ 22050

// Request text cap. The audio task copies the caller's string into a
// bounded buffer so callers don't have to keep `text` alive.
#define VOICE_TEXT_MAX 512

// I2S write chunk in frames (L+R = 2 samples each). 512 frames ≈ 23 ms.
// Small enough to respond to voice_stop() quickly without starving the DMA.
#define I2S_WRITE_FRAMES 512

// --- module state ----------------------------------------------------------

static i2s_chan_handle_t s_tx         = NULL;
static TaskHandle_t      s_audio_task = NULL;
static SemaphoreHandle_t s_request_sem = NULL;   // signals a pending utterance

// Requested/live state. All written from the audio task except where
// volatile-marked; voice_stop() flips s_stop_requested from any thread.
static char     s_pending_text[VOICE_TEXT_MAX] = "";
static volatile bool s_playing         = false;
static volatile bool s_stop_requested  = false;
static volatile uint8_t s_volume_0_21  = 18;   // seed; devcfg overrides in begin()

// --- helpers ---------------------------------------------------------------

static bool has_eleven_key(void) {
    const char *k = ELEVENLABS_API_KEY;
    return k && strlen(k) >= 10 && strncmp(k, "your-", 5) != 0;
}

// Linear gain 0..21 -> 0..1.0. Keeping it linear (not log-taper) mirrors
// the Arduino Audio library's setVolume() behaviour so the UI slider feels
// identical.
static float volume_gain(void) {
    uint8_t v = s_volume_0_21;
    if (v > 21) v = 21;
    return (float)v / 21.0f;
}

// Apply gain + expand mono samples to interleaved stereo in-place-ish. The
// caller provides `mono_in` of `frames_in` mono samples and a `stereo_out`
// buffer of at least `frames_in * 2` samples. Returns bytes written.
static size_t mono_to_stereo_gain(const int16_t *mono_in, size_t frames_in,
                                  int16_t *stereo_out, float gain) {
    for (size_t i = 0; i < frames_in; i++) {
        int32_t v = (int32_t)lroundf((float)mono_in[i] * gain);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        stereo_out[2 * i]     = (int16_t)v;
        stereo_out[2 * i + 1] = (int16_t)v;
    }
    return frames_in * 2 * sizeof(int16_t);
}

// --- I2S setup -------------------------------------------------------------

static esp_err_t i2s_install(uint32_t sample_hz) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Bigger DMA ring = smoother playback across bursty HTTP reads, at the
    // cost of a few extra KB of internal RAM.
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 240;
    // Write zero-fills when the DMA ring underruns. Without this the ring
    // keeps cycling the last descriptor forever, so the tail of the boot
    // beep loops indefinitely between the 3-tone probe and the first TTS
    // stream — which is exactly the "stuck on one sound" regression this
    // fixes.
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_LRC_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }
    return i2s_channel_enable(s_tx);
}

// --- boot beep probe -------------------------------------------------------

static void beep_tone(uint16_t freq, uint16_t duration_ms, int16_t amp) {
    if (!s_tx) return;
    const int total = (BEEP_SAMPLE_HZ * duration_ms) / 1000;
    if (total <= 0) return;
    const float phase_inc = 2.0f * (float)M_PI * (float)freq / (float)BEEP_SAMPLE_HZ;
    float phase = 0.0f;
    const int attack  = total / 6;
    const int release = total / 3;

    int16_t buf[I2S_WRITE_FRAMES * 2];   // interleaved stereo
    int idx = 0;
    for (int s = 0; s < total; s++) {
        float env = 1.0f;
        if (s < attack)               env = (float)s / (float)attack;
        else if (s > total - release) env = (float)(total - s) / (float)release;
        int16_t v = (int16_t)(sinf(phase) * (float)amp * env);
        phase += phase_inc;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        buf[idx * 2]     = v;
        buf[idx * 2 + 1] = v;
        idx++;
        if (idx >= I2S_WRITE_FRAMES) {
            size_t written = 0;
            i2s_channel_write(s_tx, buf, idx * 2 * sizeof(int16_t),
                              &written, pdMS_TO_TICKS(500));
            idx = 0;
        }
    }
    if (idx > 0) {
        size_t written = 0;
        i2s_channel_write(s_tx, buf, idx * 2 * sizeof(int16_t),
                          &written, pdMS_TO_TICKS(500));
    }
}

// --- ElevenLabs streaming --------------------------------------------------

// State threaded through esp_http_client event callbacks. The MP3-free
// path means every `on_data` byte is raw 16-bit PCM; we just expand to
// stereo and push to I2S.
typedef struct {
    // Scratch buffer for the stereo expansion. Sized to handle any single
    // on_data chunk the http client might deliver (cap at 8 KB mono -> 16
    // KB stereo). Bigger chunks just loop.
    int16_t stereo_scratch[I2S_WRITE_FRAMES * 2];
    // Mono accumulator: if a chunk arrives with an odd byte count (rare,
    // but possible at TCP boundaries), hold the straggler for the next
    // chunk so we never split a sample.
    uint8_t  half_byte;
    bool     has_half;
    size_t   total_samples_written;
    int      http_status;
} tts_ctx_t;

static esp_err_t tts_http_event(esp_http_client_event_t *evt) {
    tts_ctx_t *ctx = (tts_ctx_t *)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            // Cheap debug hook — ElevenLabs sometimes ships useful
            // "xi-character-cost" values.
            if (evt->header_key && evt->header_value &&
                (strcasecmp(evt->header_key, "content-type") == 0)) {
                ESP_LOGI(TAG, "content-type: %s", evt->header_value);
            }
            break;

        case HTTP_EVENT_ON_DATA: {
            if (s_stop_requested) {
                // Returning an error code aborts the client.
                ESP_LOGI(TAG, "stop requested; aborting stream");
                return ESP_FAIL;
            }

            // Stash 4xx/5xx body into the log instead of playing it; the
            // status code is available via esp_http_client_get_status_code
            // later but we get the body here.
            if (ctx->http_status >= 400) {
                ESP_LOGW(TAG, "err body (%d): %.*s", ctx->http_status,
                         evt->data_len > 128 ? 128 : evt->data_len,
                         (const char *)evt->data);
                break;
            }

            const uint8_t *raw = (const uint8_t *)evt->data;
            size_t         len = (size_t)evt->data_len;

            // Reassemble the trailing odd byte from last chunk, if any.
            size_t stitch = 0;
            int16_t stitched_sample = 0;
            if (ctx->has_half) {
                if (len >= 1) {
                    stitched_sample = (int16_t)((ctx->half_byte) |
                                                ((uint16_t)raw[0] << 8));
                    raw += 1;
                    len -= 1;
                    stitch = 1;
                    ctx->has_half = false;
                }
            }

            // Peel off a trailing odd byte for next time.
            bool save_half = false;
            uint8_t  half_b = 0;
            if ((len & 1) != 0) {
                half_b = raw[len - 1];
                save_half = true;
                len -= 1;
            }

            size_t mono_frames = len / 2;
            float gain = volume_gain();

            // Process the stitched sample first (one frame).
            if (stitch) {
                int16_t in[1]  = { stitched_sample };
                size_t bytes = mono_to_stereo_gain(in, 1,
                                                   ctx->stereo_scratch, gain);
                size_t written = 0;
                i2s_channel_write(s_tx, ctx->stereo_scratch, bytes,
                                  &written, pdMS_TO_TICKS(250));
            }

            // Chunk the bulk mono stream through the scratch buffer.
            const int16_t *mono = (const int16_t *)raw;
            size_t remaining = mono_frames;
            while (remaining > 0 && !s_stop_requested) {
                size_t this_chunk = remaining;
                if (this_chunk > I2S_WRITE_FRAMES) this_chunk = I2S_WRITE_FRAMES;
                size_t bytes = mono_to_stereo_gain(mono, this_chunk,
                                                   ctx->stereo_scratch, gain);
                size_t written = 0;
                esp_err_t werr = i2s_channel_write(s_tx, ctx->stereo_scratch,
                                                   bytes, &written,
                                                   pdMS_TO_TICKS(500));
                if (werr != ESP_OK) {
                    ESP_LOGW(TAG, "i2s write err: %s", esp_err_to_name(werr));
                    return ESP_FAIL;
                }
                mono += this_chunk;
                remaining -= this_chunk;
                ctx->total_samples_written += this_chunk;
            }

            if (save_half) {
                ctx->half_byte = half_b;
                ctx->has_half  = true;
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

    // Build request body via cJSON so awkward characters in `text` are
    // escaped correctly.
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

    // pcm_22050 = 22050 Hz 16-bit signed little-endian mono, matching our
    // I2S clock. No decoder needed — bytes stream straight to DMA.
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.elevenlabs.io/v1/text-to-speech/%s/stream"
             "?output_format=pcm_22050&optimize_streaming_latency=3",
             ELEVENLABS_VOICE_ID);

    tts_ctx_t ctx = {0};

    esp_http_client_config_t cfg = {
        .url              = url,
        .method           = HTTP_METHOD_POST,
        .timeout_ms       = 25000,
        .event_handler    = tts_http_event,
        .user_data        = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size      = 4096,
        .buffer_size_tx   = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        free(body);
        return false;
    }

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
        // Block until someone calls voice_speak().
        xSemaphoreTake(s_request_sem, portMAX_DELAY);

        char local[VOICE_TEXT_MAX];
        strncpy(local, s_pending_text, sizeof(local) - 1);
        local[sizeof(local) - 1] = 0;
        s_pending_text[0] = 0;

        if (!local[0]) continue;

        s_stop_requested = false;
        s_playing = true;
        (void)tts_perform(local);
        s_playing = false;
        s_stop_requested = false;
    }
}

// --- public API ------------------------------------------------------------

bool voice_begin(void) {
    if (s_tx) return true;

    // Seed volume from persisted settings so the boot beep respects the
    // user's last choice.
    s_volume_0_21 = devcfg_volume();

    esp_err_t err = i2s_install(TTS_SAMPLE_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s install: %s", esp_err_to_name(err));
        return false;
    }

    // Three-note ascending beep — loud enough to hear on a tabletop
    // speaker but short enough not to annoy during dev iteration.
    ESP_LOGI(TAG, "beep probe (you should hear 3 tones)");
    beep_tone(660,  90, 12000);
    beep_tone(990,  90, 12000);
    beep_tone(1320, 140, 12000);

    s_request_sem = xSemaphoreCreateBinary();
    if (!s_request_sem) {
        ESP_LOGE(TAG, "semaphore create failed");
        return false;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        audio_task, "voice_audio", 8192, NULL, 6, &s_audio_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "audio task create failed");
        vSemaphoreDelete(s_request_sem);
        s_request_sem = NULL;
        return false;
    }

    ESP_LOGI(TAG, "voice up @ %d Hz stereo", TTS_SAMPLE_HZ);
    return true;
}

bool voice_speak(const char *text) {
    if (!s_tx || !s_request_sem) return false;
    if (!text || !text[0]) return false;

    // Cancel anything in flight and queue the new text. The task picks it
    // up on its next loop iteration.
    s_stop_requested = true;
    strncpy(s_pending_text, text, sizeof(s_pending_text) - 1);
    s_pending_text[sizeof(s_pending_text) - 1] = 0;
    xSemaphoreGive(s_request_sem);
    return true;
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
}

void voice_loop(void) {
    // No periodic pumping needed; i2s_channel_write blocks until the DMA
    // ring is ready, and the audio task drives everything.
}

bool voice_diagnose(void) {
    if (!has_eleven_key()) {
        ESP_LOGW(TAG, "diag: no key");
        return false;
    }
    if (!wifi_sta_is_connected()) {
        ESP_LOGW(TAG, "diag: no wifi");
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.elevenlabs.io/v1/text-to-speech/%s/stream"
             "?output_format=pcm_22050",
             ELEVENLABS_VOICE_ID);

    // Minimal body: one-word text, no voice settings.
    const char *body = "{\"text\":\"hi\",\"model_id\":\"" ELEVENLABS_MODEL "\"}";

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    esp_http_client_set_header(h, "xi-api-key",   ELEVENLABS_API_KEY);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Accept",       "audio/pcm");
    esp_http_client_set_post_field(h, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    int64_t len = esp_http_client_get_content_length(h);
    esp_http_client_cleanup(h);

    ESP_LOGI(TAG, "diag: perform=%s status=%d len=%lld",
             esp_err_to_name(err), status, (long long)len);
    return (err == ESP_OK && status >= 200 && status < 300);
}
