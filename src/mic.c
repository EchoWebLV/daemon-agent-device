// ---------------------------------------------------------------------------
//  mic.c — see mic.h.
//
//  Capture pipeline:
//      I2S RX (created in voice.c) ── shared data_if ──> ES7210 codec
//                                                              │
//                                                              ▼
//                                              esp_codec_dev (TYPE_IN)
//                                                              │
//                                                              ▼
//                                          mic_record_task reads PCM
//                                          and appends to a PSRAM buffer
//
//  The recording task is spawned per-record-session, not as a long-lived
//  worker, so the codec is only opened (and PA path activated) while the
//  user is actually holding to talk.
// ---------------------------------------------------------------------------
#include "mic.h"
#include "voice.h"
#include "board.h"
#include "bus.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "mic";

#define MIC_SAMPLE_HZ        16000
#define MIC_MAX_RECORD_SECS  10
#define MIC_MAX_FRAMES       (MIC_SAMPLE_HZ * MIC_MAX_RECORD_SECS)
#define MIC_READ_FRAMES      512

static esp_codec_dev_handle_t s_dev = NULL;

static int16_t  *s_buf            = NULL;     // PSRAM, MIC_MAX_FRAMES * 2 bytes
static size_t    s_frames_written = 0;
static volatile bool s_recording  = false;
static volatile bool s_stop_req   = false;
static TaskHandle_t  s_task       = NULL;

static esp_err_t install_codec(void) {
    const audio_codec_data_if_t *data_if = voice_audio_data_if();
    if (!data_if) {
        ESP_LOGE(TAG, "voice_audio_data_if() is NULL — voice_begin must run first");
        return ESP_ERR_INVALID_STATE;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = BOARD_I2C_PORT,
        .addr       = BOARD_ES7210_I2C_ADDR,
        .bus_handle = bus_i2c(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) return ESP_FAIL;

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if  = ctrl_if,
        // Mic 1 is wired to the left mic on BOX-3. mic 2 is right; selecting
        // both gives stereo input which we'd then have to downmix. Stick to
        // a single mic for the lowest-friction path.
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&es7210_cfg);
    if (!codec_if) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    return s_dev ? ESP_OK : ESP_FAIL;
}

static void record_task(void *arg) {
    (void)arg;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = MIC_SAMPLE_HZ,
        .channel         = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed");
        s_recording = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    // Mic input gain. 30 dB was the BSP default but came back inaudible
    // for tabletop-distance speech (Whisper transcribed only "You" out of
    // 1.3 s of audio). 40 dB picks up normal-volume speech from ~0.5 m.
    esp_codec_dev_set_in_gain(s_dev, 40.0);

    int16_t scratch[MIC_READ_FRAMES];
    size_t  scratch_bytes = sizeof(scratch);

    while (!s_stop_req && s_frames_written < MIC_MAX_FRAMES) {
        esp_err_t err = esp_codec_dev_read(s_dev, scratch, scratch_bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec read err: %s", esp_err_to_name(err));
            break;
        }
        size_t avail = MIC_MAX_FRAMES - s_frames_written;
        size_t to_copy = MIC_READ_FRAMES < avail ? MIC_READ_FRAMES : avail;
        memcpy(s_buf + s_frames_written, scratch, to_copy * sizeof(int16_t));
        s_frames_written += to_copy;
    }

    esp_codec_dev_close(s_dev);
    s_recording = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mic_init(void) {
    if (s_dev) return ESP_OK;
    return install_codec();
}

esp_err_t mic_record_start(void) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (s_recording) return ESP_ERR_INVALID_STATE;

    if (!s_buf) {
        s_buf = heap_caps_malloc((size_t)MIC_MAX_FRAMES * sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_buf) {
            ESP_LOGE(TAG, "PSRAM alloc for capture buffer failed");
            return ESP_ERR_NO_MEM;
        }
    }
    s_frames_written = 0;
    s_stop_req       = false;
    s_recording      = true;

    BaseType_t ok = xTaskCreatePinnedToCore(
        record_task, "mic_record", 4096, NULL, 6, &s_task, 0);
    if (ok != pdPASS) {
        s_recording = false;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "recording (cap %d s)", MIC_MAX_RECORD_SECS);
    return ESP_OK;
}

esp_err_t mic_record_stop(int16_t **out_pcm, size_t *out_frames) {
    if (!s_recording && !s_task) {
        // Already stopped. Drain whatever's in the buffer.
        if (s_frames_written == 0) return ESP_ERR_INVALID_STATE;
    } else {
        s_stop_req = true;
        // Wait for the record task to drain. Bound the wait so a stuck
        // codec read doesn't hang the caller (LVGL UI thread).
        for (int i = 0; i < 50 && s_recording; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_recording) {
            ESP_LOGW(TAG, "record task didn't stop within 1 s; orphaning");
        }
    }

    int16_t *pcm = s_buf;
    size_t   frames = s_frames_written;
    s_buf = NULL;
    s_frames_written = 0;

    if (out_pcm)    *out_pcm    = pcm;
    if (out_frames) *out_frames = frames;

    ESP_LOGI(TAG, "captured %u frames (%.1f s)",
             (unsigned)frames, (float)frames / (float)MIC_SAMPLE_HZ);
    return ESP_OK;
}

void mic_buffer_free(int16_t *pcm) {
    if (pcm) heap_caps_free(pcm);
}

bool mic_is_recording(void) {
    return s_recording;
}
