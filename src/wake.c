// ---------------------------------------------------------------------------
//  wake.c — see wake.h.
//
//  The shape of the pipeline mirrors esp-skainet's
//  examples/wake_word_detection/afe/main/main.c (public domain): a feed
//  task and a detect task, both pinned to opposite cores, both running
//  flat-out while `s_running` is true. We diverge on:
//
//   * Codec source. esp-skainet uses the BSP's `esp_get_feed_data`; we
//     own `bsp_audio_codec_microphone_init` directly so the existing
//     mic.c PTT path can stay on the same handle and the new path can
//     share the bytes (see wake_capture_start/stop and the append step
//     inside feed_task).
//   * Channel handling. ES7210 on BOX-3 always clocks two slots (MIC1
//     + MIC2). We open the codec stereo and sum L+R to mono in software,
//     same as the prior mic.c. AFE input is "M" so it expects 1 channel.
//   * Capture. AFE has no concept of "give me the audio that just
//     finished" — wake_word_detection only logs the detection. We add
//     a circular-ish capture buffer that the feed task appends into
//     while a capture is active, plus a VAD-driven endpoint inside the
//     detect task that swaps the buffer out and hands it to the
//     on_speech callback when the user pauses.
//
//  Concurrency: feed (core 0) and detect (core 1) both touch s_buf and
//  s_buf_frames. We hold a portMUX_TYPE around any read-modify-write,
//  including the buffer hand-off when a capture endpoints. Critical
//  sections are short (memcpy of one AFE chunk, ≈ 1 KB) so the lock
//  contention is invisible against the 32 ms feed cadence.
// ---------------------------------------------------------------------------
#include "wake.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_codec_dev.h"
#include "bsp/esp-box-3.h"

// ESP-SR public headers.
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "wake";

#define WAKE_SAMPLE_HZ      16000
#define WAKE_CHANNELS_RAW   2                // ES7210 dual-mic wire format
#define WAKE_MAX_RECORD_S   10
#define WAKE_MAX_FRAMES     (WAKE_SAMPLE_HZ * WAKE_MAX_RECORD_S)
#define WAKE_MAX_BYTES      (WAKE_MAX_FRAMES * (int)sizeof(int16_t))

// VAD silence threshold: end a wake-driven capture after this many
// continuous milliseconds of VAD-reported silence. 1.2 s reads as a
// natural "I'm done" pause without truncating mid-thought hesitations.
#define WAKE_VAD_SILENCE_MS 1200
// Hard cap on capture duration — a stuck VAD or a runaway monologue
// shouldn't be able to grow the recording past WAKE_MAX_RECORD_S.
#define WAKE_MAX_CAPTURE_MS (WAKE_MAX_RECORD_S * 1000)
// Discard captures shorter than this — accidental wakes that immediately
// fall silent. 100 ms = 1600 frames at 16 kHz.
#define WAKE_MIN_FRAMES     1600
// AFE in low-cost mode publishes ~32 ms chunks. Used to accumulate the
// VAD silence counter — we don't query AFE for the exact value because
// a small drift (2-3 ms) on the silence threshold doesn't matter.
#define WAKE_FETCH_CHUNK_MS 32

typedef enum {
    CAP_NONE = 0,
    CAP_PTT,        // diverted by mic_record_start; stop is manual
    CAP_WAKE_VAD,   // wake-triggered; stop is automatic via VAD endpoint
} capture_mode_t;

static portMUX_TYPE              s_mux         = portMUX_INITIALIZER_UNLOCKED;
static volatile capture_mode_t   s_mode        = CAP_NONE;
static volatile bool             s_enabled     = true;
static volatile bool             s_running     = false;

static int16_t                  *s_buf         = NULL;
static volatile size_t           s_buf_frames  = 0;

static esp_codec_dev_handle_t    s_codec       = NULL;
static const esp_afe_sr_iface_t *s_afe         = NULL;
static esp_afe_sr_data_t        *s_afe_data    = NULL;

static TaskHandle_t              s_feed_task   = NULL;
static TaskHandle_t              s_detect_task = NULL;

static wake_detect_cb_t          s_on_wake     = NULL;
static wake_speech_cb_t          s_on_speech   = NULL;

static uint32_t                  s_silence_ms       = 0;
static uint32_t                  s_capture_start_ms = 0;

// ---------------------------------------------------------------------------
//  Mode transitions. Held under s_mux because feed (core 0) and detect
//  (core 1) both observe and act on s_mode.
// ---------------------------------------------------------------------------
static bool try_enter_mode(capture_mode_t want) {
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_mode == CAP_NONE && s_buf) {
        s_mode             = want;
        s_buf_frames       = 0;
        s_silence_ms       = 0;
        s_capture_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

// Detach the active capture buffer for hand-off. Caller has pre-allocated
// `replacement` so the next capture has somewhere to land — we can't
// malloc inside a portENTER_CRITICAL section. *out_frames is the count
// captured before the swap.
static void swap_buffer_and_clear(int16_t *replacement,
                                  int16_t **out_pcm, size_t *out_frames) {
    portENTER_CRITICAL(&s_mux);
    *out_pcm    = s_buf;
    *out_frames = s_buf_frames;
    s_buf         = replacement;
    s_buf_frames  = 0;
    s_mode        = CAP_NONE;
    s_silence_ms  = 0;
    portEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------------------
//  Feed task — codec read → sum to mono → AFE feed (+ append to capture
//  buffer if a capture is active). Pinned to core 0 alongside the audio
//  output path; AFE's internal pre-processing fits comfortably in the
//  per-chunk window between codec reads.
// ---------------------------------------------------------------------------
static void feed_task(void *arg) {
    (void)arg;

    int chunksize = s_afe->get_feed_chunksize(s_afe_data);
    int afe_nch   = s_afe->get_feed_channel_num(s_afe_data);
    if (afe_nch != 1) {
        // Sanity warning — our input format is "M" so AFE should ask for
        // exactly 1 channel. If this ever fires, the format string in
        // wake_init disagrees with what AFE actually negotiated.
        ESP_LOGW(TAG, "AFE expects %d channels but feeder is mono", afe_nch);
    }

    // Stereo scratch buffer — we read 2 channels from ES7210 then sum.
    // Mono scratch buffer — what AFE sees and what we append to s_buf.
    size_t   stereo_bytes = (size_t)chunksize * WAKE_CHANNELS_RAW * sizeof(int16_t);
    int16_t *stereo = heap_caps_malloc(stereo_bytes,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono   = heap_caps_malloc((size_t)chunksize * sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stereo || !mono) {
        ESP_LOGE(TAG, "feed scratch alloc failed (chunksize=%d)", chunksize);
        if (stereo) heap_caps_free(stereo);
        if (mono)   heap_caps_free(mono);
        s_feed_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "feed task up (chunksize=%d frames, ~%dms)",
             chunksize, (int)((chunksize * 1000) / WAKE_SAMPLE_HZ));

    while (s_running) {
        esp_err_t err = esp_codec_dev_read(s_codec, stereo, stereo_bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec read err: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Sum L+R → mono. Both ES7210 mics on BOX-3 hear the same ambient
        // audio so summing doubles the SNR vs picking one slot. Halve to
        // avoid int16 overflow (matches mic.c's previous behavior).
        for (int i = 0; i < chunksize; i++) {
            int32_t mix = ((int32_t)stereo[i * 2] + (int32_t)stereo[i * 2 + 1]) / 2;
            mono[i] = (int16_t)mix;
        }

        s_afe->feed(s_afe_data, mono);

        // Append to capture buffer iff a capture is active. Held under
        // s_mux so detect_task's endpoint swap can't race the memcpy.
        portENTER_CRITICAL(&s_mux);
        if (s_mode != CAP_NONE && s_buf) {
            size_t avail = (size_t)WAKE_MAX_FRAMES - s_buf_frames;
            size_t copy  = (size_t)chunksize < avail ? (size_t)chunksize : avail;
            if (copy > 0) {
                memcpy(s_buf + s_buf_frames, mono, copy * sizeof(int16_t));
                s_buf_frames += copy;
            }
        }
        portEXIT_CRITICAL(&s_mux);
    }

    heap_caps_free(stereo);
    heap_caps_free(mono);
    s_feed_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
//  Detect task — AFE fetch → handle wake events + drive VAD endpoint
//  for wake-triggered captures. Pinned to core 1.
// ---------------------------------------------------------------------------
static void detect_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "detect task up");

    while (s_running) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // ---- Wake event ------------------------------------------------
        if (s_enabled && res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected (model_idx=%d, word_idx=%d)",
                     res->wakenet_model_index, res->wake_word_index);
            if (try_enter_mode(CAP_WAKE_VAD)) {
                if (s_on_wake) s_on_wake();
            } else {
                ESP_LOGI(TAG, "wake fired but capture already in flight");
            }
        }

        // ---- VAD endpoint (only in WAKE_VAD mode) ---------------------
        if (s_mode == CAP_WAKE_VAD) {
            if (res->vad_state == VAD_SILENCE) {
                s_silence_ms += WAKE_FETCH_CHUNK_MS;
            } else {
                s_silence_ms = 0;
            }

            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t cap_ms = now_ms - s_capture_start_ms;

            bool by_silence = s_silence_ms >= WAKE_VAD_SILENCE_MS;
            bool by_max     = cap_ms >= WAKE_MAX_CAPTURE_MS;

            if (by_silence || by_max) {
                ESP_LOGI(TAG, "endpoint: %s (cap=%lums, silence=%lums)",
                         by_silence ? "silence" : "max-duration",
                         (unsigned long)cap_ms,
                         (unsigned long)s_silence_ms);

                // Pre-allocate the next capture buffer outside the
                // critical section. If this fails we leave the engine
                // in WAKE_VAD and try again on the next fetch — better
                // than dropping the live capture.
                int16_t *next_buf = heap_caps_malloc(WAKE_MAX_BYTES,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!next_buf) {
                    ESP_LOGE(TAG, "next-capture buf alloc failed; retrying");
                    continue;
                }

                int16_t *pcm    = NULL;
                size_t   frames = 0;
                swap_buffer_and_clear(next_buf, &pcm, &frames);

                if (pcm && frames >= WAKE_MIN_FRAMES && s_on_speech) {
                    s_on_speech(pcm, frames);
                } else {
                    if (frames < WAKE_MIN_FRAMES) {
                        ESP_LOGI(TAG, "capture below min (%u frames) — discarded",
                                 (unsigned)frames);
                    }
                    if (pcm) heap_caps_free(pcm);
                }
            }
        }
    }

    s_detect_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------
esp_err_t wake_init(wake_detect_cb_t on_wake, wake_speech_cb_t on_speech) {
    if (s_running) return ESP_OK;

    // 1) Codec — open continuously at 16 kHz stereo so feed_task can
    // pull a steady stream. Same gain as mic.c's prior behavior so the
    // captured audio matches what STT was tuned against.
    s_codec = bsp_audio_codec_microphone_init();
    if (!s_codec) {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = WAKE_SAMPLE_HZ,
        .channel         = WAKE_CHANNELS_RAW,
        .bits_per_sample = 16,
    };
    esp_codec_dev_set_in_gain(s_codec, 42.0);
    if (esp_codec_dev_open(s_codec, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "codec open continuous (handle=%p)", s_codec);

    // 2) Initial capture buffer in PSRAM. Reused across captures via the
    // swap-and-clear hand-off — when a capture finishes, detect_task
    // pre-allocates a replacement and swaps under the lock.
    s_buf = heap_caps_malloc(WAKE_MAX_BYTES,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        ESP_LOGE(TAG, "PSRAM alloc for capture buffer (%d bytes) failed",
                 (int)WAKE_MAX_BYTES);
        return ESP_ERR_NO_MEM;
    }
    s_buf_frames = 0;

    // 3) Models — esp_srmodel_init scans the "model" SPIFFS partition
    // and returns descriptors for every WakeNet/VADNet packed in. The
    // WakeNet selection comes from sdkconfig (CONFIG_SR_WN_*); only the
    // selected model is included by the build system, so models->num
    // is small (1-2 entries).
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || models->num == 0) {
        ESP_LOGE(TAG, "no ESP-SR models in 'model' partition; "
                      "did the build flash srmodels?");
        return ESP_FAIL;
    }
    for (int i = 0; i < models->num; i++) {
        ESP_LOGI(TAG, "  loaded model: %s", models->model_name[i]);
    }

    // 4) AFE — single mic ("M"), no AEC reference, low-cost mode.
    // afe_config_init picks defaults: VAD on, noise-suppression on,
    // wake-word on. esp_afe_handle_from_config returns the SR variant
    // (vs the voice-comm variant which is wired for AEC).
    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }
    if (cfg->wakenet_model_name) {
        ESP_LOGI(TAG, "  AFE wake word: %s", cfg->wakenet_model_name);
    }

    s_afe      = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return ESP_FAIL;
    }

    s_on_wake   = on_wake;
    s_on_speech = on_speech;
    s_enabled   = true;
    s_running   = true;

    // 5) Spawn the two pumps. Stack sizes mirror esp-skainet's example;
    // tasks are pinned so the cores share the audio + AFE work without
    // cache thrash. Priority 5 matches the rest of our long-running
    // workers (mic record task pre-refactor, voice audio task).
    BaseType_t ok1 = xTaskCreatePinnedToCore(feed_task,   "wake_feed", 4096,
                                             NULL, 5, &s_feed_task,   0);
    BaseType_t ok2 = xTaskCreatePinnedToCore(detect_task, "wake_det",  4096,
                                             NULL, 5, &s_detect_task, 1);
    if (ok1 != pdPASS || ok2 != pdPASS) {
        ESP_LOGE(TAG, "task spawn failed (feed=%d detect=%d)", ok1, ok2);
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "wake engine running");
    return ESP_OK;
}

esp_err_t wake_capture_start(void) {
    if (!s_running) return ESP_ERR_INVALID_STATE;
    if (!try_enter_mode(CAP_PTT)) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "PTT capture started");
    return ESP_OK;
}

esp_err_t wake_capture_stop(int16_t **out_pcm, size_t *out_frames) {
    if (!s_running) return ESP_ERR_INVALID_STATE;
    if (s_mode != CAP_PTT) return ESP_ERR_INVALID_STATE;

    int16_t *next_buf = heap_caps_malloc(WAKE_MAX_BYTES,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next_buf) {
        ESP_LOGE(TAG, "next-capture buf alloc failed");
        return ESP_ERR_NO_MEM;
    }

    int16_t *pcm    = NULL;
    size_t   frames = 0;
    swap_buffer_and_clear(next_buf, &pcm, &frames);

    if (out_pcm)    *out_pcm    = pcm;
    if (out_frames) *out_frames = frames;

    ESP_LOGI(TAG, "PTT capture stopped (%u frames, %.2f s)",
             (unsigned)frames, (float)frames / (float)WAKE_SAMPLE_HZ);
    return ESP_OK;
}

bool wake_capture_is_active(void) {
    return s_mode != CAP_NONE;
}

void wake_set_enabled(bool enabled) { s_enabled = enabled; }
bool wake_is_enabled(void)          { return s_enabled; }
