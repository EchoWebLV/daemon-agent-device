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

#define MIC_SAMPLE_HZ        16000
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

    // chatgpt_demo opens the codec at 16 kHz / 16-bit / 2-channel (stereo).
    // ES7210 has 4 mics; the BSP default selects MIC1+MIC2 which appear on
    // the I2S left and right slots. We average L+R into a single mono
    // sample per frame so STT gets a 16 kHz mono PCM stream.
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = MIC_SAMPLE_HZ,
        .channel         = 2,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed");
        s_recording = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    // ES7210's open() hardcodes 30 dB on all channels. We adjust afterward.
    // Pre-open calls return INVALID_STATE silently because the underlying
    // _es7210_set_channel_gain refuses to write to a closed codec.
    // 12 dB is below the open-default 30 to avoid clipping on tabletop
    // speech which we measured peaking at full-scale at 30 dB.
    esp_codec_dev_set_in_gain(s_dev, 12.0);

    int16_t scratch[MIC_READ_FRAMES * 2];
    size_t  scratch_bytes = sizeof(scratch);
    int16_t peak_l_overall = 0, peak_r_overall = 0;

    while (!s_stop_req && s_frames_written < MIC_MAX_FRAMES) {
        esp_err_t err = esp_codec_dev_read(s_dev, scratch, scratch_bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec read err: %s", esp_err_to_name(err));
            break;
        }
        size_t avail = MIC_MAX_FRAMES - s_frames_written;
        size_t to_copy = MIC_READ_FRAMES < avail ? MIC_READ_FRAMES : avail;
        int16_t *dst = s_buf + s_frames_written;
        // Diagnostic: track L and R peaks across the whole capture so we
        // can see if one channel is dead. Use LEFT only into the mono
        // buffer — averaging halves volume if R is silent.
        for (size_t i = 0; i < to_copy; i++) {
            int16_t l = scratch[i * 2];
            int16_t r = scratch[i * 2 + 1];
            int16_t la = l < 0 ? -l : l;
            int16_t ra = r < 0 ? -r : r;
            if (la > peak_l_overall) peak_l_overall = la;
            if (ra > peak_r_overall) peak_r_overall = ra;
            dst[i] = l;
        }
        s_frames_written += to_copy;
    }
    ESP_LOGI(TAG, "channel peaks during capture: L=%d  R=%d",
             (int)peak_l_overall, (int)peak_r_overall);

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
