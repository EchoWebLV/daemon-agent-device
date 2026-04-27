// ---------------------------------------------------------------------------
//  Voice playback — ElevenLabs PCM stream → ES8311 codec on the BOX-3.
//
//  Pipeline (after voice_begin):
//      voice_speak("...")
//        → audio_task wakes
//        → ElevenLabs HTTPS POST (/v1/text-to-speech/<voice>/stream,
//          output_format=pcm_22050)
//        → on each HTTP_EVENT_ON_DATA, raw 16-bit signed-LE mono PCM
//          frames stream into esp_codec_dev_write
//        → ES8311 codec (esp_codec_dev) drives I2S out + PA enable
//
//  Public API (voice_begin / voice_speak / voice_stop / voice_is_speaking
//  / voice_set_volume / voice_diagnose) is unchanged from the previous
//  PCM5101-DAC implementation, so app_main + ui.c don't have to know.
//
//  Hardware notes (BOX-3):
//   - Codec ctrl on the shared I2C bus (bus_i2c() from bus.c) at 0x18.
//   - I2S on I2S_NUM_0 with MCLK + BCLK + LRCK + DOUT pins from board.h.
//   - PA enable on BOARD_AUDIO_PIN_PA_EN; the ES8311 driver toggles it
//     itself via the gpio_if when the codec opens/closes, so we don't
//     bit-bang it from here.
// ---------------------------------------------------------------------------
#include "voice.h"
#include "secrets.h"
#include "wifi_sta.h"
#include "devcfg.h"
#include "board.h"
#include "bus.h"

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

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "cJSON.h"

static const char *TAG = "voice";

// 16 kHz mono is the lowest common rate that works for both ES8311 (TTS
// out) and ES7210 (mic in). We have to share the I2S BCLK/LRCK between the
// two codecs on the BOX-3, so they must run at the same effective sample
// rate. Whisper-class STT models also expect 16 kHz, so this is a no-loss
// choice for voice input. ElevenLabs returns pcm_16000 cleanly enough.
#define TTS_SAMPLE_HZ  16000
#define BEEP_SAMPLE_HZ 16000
#define VOICE_TEXT_MAX 512

// Codec write chunk in frames. ~32 ms at 16000 Hz mono.
#define CODEC_WRITE_FRAMES 512

// --- module state ----------------------------------------------------------

static i2s_chan_handle_t       s_i2s_tx     = NULL;
static i2s_chan_handle_t       s_i2s_rx     = NULL;
// Shared codec data_if — wraps both TX and RX channels. Voice (ES8311 OUT)
// and mic (ES7210 IN) both use this same handle through their respective
// codec configs. mic.c grabs it via voice_audio_data_if() so we don't
// double-create.
static const audio_codec_data_if_t *s_audio_data_if = NULL;
static esp_codec_dev_handle_t  s_codec      = NULL;   // OUT (speaker) handle
static TaskHandle_t            s_audio_task = NULL;
static SemaphoreHandle_t       s_request_sem = NULL;

static char     s_pending_text[VOICE_TEXT_MAX] = "";
static volatile bool s_playing         = false;
static volatile bool s_stop_requested  = false;
static volatile uint8_t s_volume_0_21  = 0;

// --- helpers ---------------------------------------------------------------

static bool has_eleven_key(void) {
    const char *k = ELEVENLABS_API_KEY;
    return k && strlen(k) >= 10 && strncmp(k, "your-", 5) != 0;
}

// 0..21 → 0..100 for the codec's percent-volume API. Keep linear (not log)
// because the small tabletop speaker has narrow useful range and a log
// taper makes the bottom half feel dead.
static int volume_percent(void) {
    uint8_t v = s_volume_0_21;
    if (v > 21) v = 21;
    return (int)((v * 100) / 21);
}

// --- codec bring-up --------------------------------------------------------

static esp_err_t i2s_install(uint32_t sample_hz) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num         = 8;
    chan_cfg.dma_frame_num        = 240;
    chan_cfg.auto_clear_after_cb  = true;

    // Both TX (ES8311 / speaker) and RX (ES7210 / mic) channels created in
    // a single call so they share BCLK/LRCK on the BOX-3's wired bus. Mic
    // capture (mic.c) opens the RX side later via the shared data_if.
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx);
    if (err != ESP_OK) return err;

    // ES8311/ES7210 want Philips slot format, 16-bit, mono. The codecs
    // route mono samples to their internal stereo paths automatically.
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_PIN_MCLK,
            .bclk = BOARD_I2S_PIN_BCLK,
            .ws   = BOARD_I2S_PIN_LRCK,
            .dout = BOARD_I2S_PIN_DOUT,
            .din  = BOARD_I2S_PIN_DIN,
            .invert_flags = { 0 },
        },
    };
    err = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_init_std_mode(s_i2s_rx, &std_cfg);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(s_i2s_tx);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(s_i2s_rx);
    if (err != ESP_OK) goto fail;
    return ESP_OK;

fail:
    if (s_i2s_tx) { i2s_del_channel(s_i2s_tx); s_i2s_tx = NULL; }
    if (s_i2s_rx) { i2s_del_channel(s_i2s_rx); s_i2s_rx = NULL; }
    return err;
}

static esp_err_t codec_install(void) {
    // I2S data interface for the codec — wraps BOTH tx and rx handles so
    // mic.c can share the same data_if for the ES7210 input path.
    audio_codec_i2s_cfg_t i2s_if_cfg = {
        .port      = BOARD_I2S_PORT,
        .tx_handle = s_i2s_tx,
        .rx_handle = s_i2s_rx,
    };
    s_audio_data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    if (!s_audio_data_if) return ESP_FAIL;
    const audio_codec_data_if_t *data_if = s_audio_data_if;

    // I2C control interface — sources the bus from bus.c so we share
    // I2C_NUM_0 with the touch IC.
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = BOARD_I2C_PORT,
        .addr       = BOARD_ES8311_I2C_ADDR,
        .bus_handle = bus_i2c(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) return ESP_FAIL;

    // GPIO interface — used by the codec driver to toggle PA enable when
    // play starts/stops.
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!gpio_if) return ESP_FAIL;

    // Hardware-gain hints for volume scaling. Values from the BOX-3 BSP
    // (5.0 V PA, 3.3 V codec DAC).
    esp_codec_dev_hw_gain_t hw_gain = {
        .pa_voltage         = 5.0,
        .codec_dac_voltage  = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if      = ctrl_if,
        .gpio_if      = gpio_if,
        .codec_mode   = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin       = BOARD_AUDIO_PIN_PA_EN,
        .pa_reverted  = false,
        .master_mode  = false,
        .use_mclk     = true,
        .digital_mic  = false,
        .invert_mclk  = false,
        .invert_sclk  = false,
        .hw_gain      = hw_gain,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (!codec_if) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    return s_codec ? ESP_OK : ESP_FAIL;
}

static esp_err_t codec_open_for_speak(void) {
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = TTS_SAMPLE_HZ,
        .channel         = 1,                  // mono — codec handles fan-out
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
    int16_t  scratch[CODEC_WRITE_FRAMES];   // mono write chunk
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

            // Stitch a stranded byte from a previous chunk if any.
            if (ctx->has_half && len >= 1) {
                int16_t stitched = (int16_t)((ctx->half_byte) |
                                             ((uint16_t)raw[0] << 8));
                esp_codec_dev_write(s_codec, &stitched, sizeof(stitched));
                ctx->total_samples_written += 1;
                raw += 1;
                len -= 1;
                ctx->has_half = false;
            }

            // Peel off a trailing odd byte for next time.
            if (len & 1) {
                ctx->half_byte = raw[len - 1];
                ctx->has_half  = true;
                len -= 1;
            }

            // Bulk write in CODEC_WRITE_FRAMES chunks. Codec handles
            // volume scaling internally via set_out_vol; we don't fold
            // gain here.
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
    // pcm_16000 matches our I2S clock domain (shared with the mic input
     // path). 16 kHz speech audio is intelligible; the loss vs 22050 is
     // imperceptible on the small BOX-3 speaker.
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

        char local[VOICE_TEXT_MAX];
        strncpy(local, s_pending_text, sizeof(local) - 1);
        local[sizeof(local) - 1] = 0;
        s_pending_text[0] = 0;

        if (!local[0]) continue;

        s_stop_requested = false;
        s_playing = true;
        // Reopen the codec each utterance so changes to volume / sample
        // rate take effect; close on done so the PA powers down.
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

    esp_err_t err = i2s_install(TTS_SAMPLE_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s install: %s", esp_err_to_name(err));
        return false;
    }
    err = codec_install();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec install: %s", esp_err_to_name(err));
        if (s_i2s_tx) { i2s_del_channel(s_i2s_tx); s_i2s_tx = NULL; }
        return false;
    }

    // Three-note ascending probe so a dead codec / dead speaker is obvious.
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

    ESP_LOGI(TAG, "voice up @ %d Hz mono via ES8311", TTS_SAMPLE_HZ);
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

bool voice_diagnose(void) {
    if (!has_eleven_key()) {
        ESP_LOGW(TAG, "diag: no key");
        return false;
    }
    if (!wifi_sta_is_connected()) {
        ESP_LOGW(TAG, "diag: no wifi");
        return false;
    }
    return true;
}

const audio_codec_data_if_t *voice_audio_data_if(void) {
    return s_audio_data_if;
}
