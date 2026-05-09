// ---------------------------------------------------------------------------
//  wake.h — always-on wake-word engine for the BOX-3.
//
//  Owns the ES7210 microphone codec continuously (replaces mic.c's prior
//  open-on-PTT lifecycle) and runs Espressif's AFE pipeline plus a
//  WakeNet9 model loaded from the `model` SPIFFS partition. Two tasks
//  drive the pipeline: a feed task pumps codec samples into AFE, and a
//  detect task fetches results, fires `on_wake` when the wake word lands,
//  and watches the VAD state to decide when the user has finished speaking
//  (then fires `on_speech` with the captured PCM).
//
//  The same engine also services PTT: `wake_capture_start/stop` is the
//  back end behind `mic_record_start/stop`, so the codec doesn't have to
//  thrash open/close per press. AFE keeps running during PTT capture so
//  the wake word is still detectable mid-press (harmless — `try_enter_mode`
//  refuses to double-capture).
//
//  Wake word: configured via `CONFIG_SR_WN_*` in sdkconfig.defaults. We
//  ship the English "Hi Lexin" placeholder while Espressif's WakeNet
//  generator service produces the custom "Hey Daemon" model (1-2 weeks).
// ---------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fires the moment WAKENET_DETECTED lands, BEFORE the user finishes
// speaking. Use this to clear in-flight TTS / cancel an in-flight reply
// so the user is heard. May be called from the AFE detect task; the
// implementation is responsible for any task hops it needs (LVGL, etc.).
typedef void (*wake_detect_cb_t)(void);

// Fires after the wake-triggered utterance has been captured to its end
// (VAD silence detected, or the 10-second hard cap reached). The pcm
// buffer is heap_caps_malloc'd in PSRAM; the callback OWNS it and must
// free via mic_buffer_free / heap_caps_free.
typedef void (*wake_speech_cb_t)(int16_t *pcm, size_t frames);

// Bring up ESP-SR (model load + AFE create), open the ES7210 codec
// continuously, and spawn the feed + detect tasks. Either callback may
// be NULL (e.g. pass NULL to on_wake during early bringup if you only
// want the wake event after capture). Returns ESP_OK on success; on
// failure the engine is not running, no callbacks ever fire, and the
// PTT path falls back to whatever mic.c's shim returns.
esp_err_t wake_init(wake_detect_cb_t on_wake, wake_speech_cb_t on_speech);

// Capture diversion (PTT path). Starts routing mono frames into the
// shared PSRAM buffer; AFE keeps running so wake detection still works.
// _stop returns the buffer (transfer of ownership) and the frame count
// captured up to the stop call. AFE-driven captures use the same buffer
// and protocol — see the on_speech callback. Both paths are mutually
// exclusive: the second caller gets ESP_ERR_INVALID_STATE.
esp_err_t wake_capture_start(void);
esp_err_t wake_capture_stop(int16_t **out_pcm, size_t *out_frames);
bool      wake_capture_is_active(void);

// Toggle wake-word detection. The feed pipeline keeps running so an
// in-flight capture's VAD endpoint still works; only WAKENET_DETECTED
// events are suppressed. Used by voice.c around TTS playback so the
// device's own audio doesn't false-trigger the detector.
void wake_set_enabled(bool enabled);
bool wake_is_enabled(void);

#ifdef __cplusplus
}
#endif
