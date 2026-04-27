// ---------------------------------------------------------------------------
//  mic.c — see mic.h.
//
//  Uses the BOX-3 BSP's microphone codec init directly. The BSP returns
//  an esp_codec_dev handle wired to ES7210 over the same I2S port the
//  speaker uses; bsp_audio_codec_microphone_init shares the underlying
//  I2S setup with bsp_audio_codec_speaker_init by design.
//
//  The capture buffer lives in PSRAM (16 kHz mono * 16-bit * 10 s = ~320 KB),
//  which is too big for internal RAM but fine in the BOX-3's 16 MB Octal
//  PSRAM pool.
// ---------------------------------------------------------------------------
#include "mic.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "esp_codec_dev.h"
#include "bsp/esp-box-3.h"

static const char *TAG = "mic";

// 16000 Hz, captured stereo and decimated to mono.
//
// BOX-3 has a *dual* mic on ES7210; the codec puts MIC1 on the left I2S
// slot and MIC2 on the right. If we ask esp_codec_dev for channel=1 it
// reconfigures the I2S peripheral with I2S_SLOT_MODE_MONO (single slot
// per WS period) — but ES7210 keeps clocking out two slots, so the bit
// stream slips and we get glitchy noise. So we open at channel=2,
// matching what the codec actually sends, and drop the right channel
// (MIC2) in software.
#define MIC_SAMPLE_HZ        16000
#define MIC_CHANNELS_RAW     2
#define MIC_MAX_RECORD_SECS  10
#define MIC_MAX_FRAMES       (MIC_SAMPLE_HZ * MIC_MAX_RECORD_SECS)
#define MIC_READ_FRAMES      512

static esp_codec_dev_handle_t s_dev = NULL;

static int16_t  *s_buf            = NULL;
static size_t    s_frames_written = 0;
static volatile bool s_recording  = false;
static volatile bool s_stop_req   = false;
static TaskHandle_t  s_task       = NULL;

static void record_task(void *arg) {
    (void)arg;

    // BSP example pattern (canonical for plain capture):
    //   esp_codec_dev_set_in_gain(...)   BEFORE open
    //   esp_codec_dev_open(channel=2)    stereo (matches ES7210 wire format)
    //   esp_codec_dev_read into an interleaved L/R buffer
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = MIC_SAMPLE_HZ,
        .channel         = MIC_CHANNELS_RAW,   // 2 — see header comment
        .bits_per_sample = 16,
    };
    esp_codec_dev_set_in_gain(s_dev, 42.0);   // matches BSP example
    if (esp_codec_dev_open(s_dev, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed");
        s_recording = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Stereo scratch — twice as many int16 elements per frame.
    int16_t scratch[MIC_READ_FRAMES * MIC_CHANNELS_RAW];
    size_t  scratch_bytes = sizeof(scratch);
    int16_t peak_l = 0, peak_r = 0;

    while (!s_stop_req && s_frames_written < MIC_MAX_FRAMES) {
        esp_err_t err = esp_codec_dev_read(s_dev, scratch, scratch_bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec read err: %s", esp_err_to_name(err));
            break;
        }
        size_t avail = MIC_MAX_FRAMES - s_frames_written;
        size_t to_copy = MIC_READ_FRAMES < avail ? MIC_READ_FRAMES : avail;
        int16_t *dst = s_buf + s_frames_written;
        // Sum L+R into mono — both ES7210 mics on BOX-3 capture the same
        // ambient audio, so summing doubles the SNR vs picking one slot
        // (and dodges the "is MIC1 on L or R?" question entirely).
        for (size_t i = 0; i < to_copy; i++) {
            int16_t l  = scratch[i * 2];
            int16_t r  = scratch[i * 2 + 1];
            int16_t al = l < 0 ? (int16_t)-l : l;
            int16_t ar = r < 0 ? (int16_t)-r : r;
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
            // Sign-extending sum then halve — avoids int16 overflow.
            int32_t mix = ((int32_t)l + (int32_t)r) / 2;
            dst[i] = (int16_t)mix;
        }
        s_frames_written += to_copy;
    }
    ESP_LOGI(TAG, "capture peak: L=%d  R=%d", (int)peak_l, (int)peak_r);

    esp_codec_dev_close(s_dev);
    s_recording = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mic_init(void) {
    if (s_dev) return ESP_OK;
    s_dev = bsp_audio_codec_microphone_init();
    if (!s_dev) {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "mic up via BSP/ES7210 (handle=%p)", s_dev);
    return ESP_OK;
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
        record_task, "mic_record", 8192, NULL, 6, &s_task, 0);
    if (ok != pdPASS) {
        s_recording = false;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "recording (cap %d s)", MIC_MAX_RECORD_SECS);
    return ESP_OK;
}

esp_err_t mic_record_stop(int16_t **out_pcm, size_t *out_frames) {
    if (!s_recording && !s_task) {
        if (s_frames_written == 0) return ESP_ERR_INVALID_STATE;
    } else {
        s_stop_req = true;
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
