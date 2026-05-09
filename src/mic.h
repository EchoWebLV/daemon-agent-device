// ---------------------------------------------------------------------------
//  mic.h — ES7210 microphone capture for the ESP32-S3-BOX-3.
//
//  Push-to-talk path:
//      wake_init() once at boot (owns the codec; mic_init is a no-op)
//      → user long-presses creature → mic_record_start()
//      → user releases → mic_record_stop(&pcm, &frames) returns the
//        recorded 16 kHz 16-bit mono PCM
//      → caller hands the buffer to stt.c, which uploads + transcribes,
//        and then frees with mic_buffer_free()
//
//  Implementation lives in wake.c — this header is the legacy facade
//  used by creature_screen.c's PTT handlers. The wake-word engine and
//  PTT share the same continuously-open codec and the same PSRAM
//  capture buffer; wake.c's mode interlock prevents double-capture.
// ---------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mic_init(void);

// Begin capturing into an internally-allocated PSRAM buffer. Caps at
// MIC_MAX_RECORD_SECS — any audio past that is dropped, not appended.
// Returns ESP_ERR_INVALID_STATE if capture is already in flight.
esp_err_t mic_record_start(void);

// Stop the in-flight capture and yield the buffer + frame count to the
// caller. On success the caller OWNS *out_pcm and must free it via
// mic_buffer_free(). Returns ESP_ERR_INVALID_STATE if no capture is
// running.
esp_err_t mic_record_stop(int16_t **out_pcm, size_t *out_frames);

// Free a buffer returned by mic_record_stop. Safe with NULL.
void mic_buffer_free(int16_t *pcm);

// True while a recording is in flight. Useful for UI state machines that
// want to show a "listening..." indicator.
bool mic_is_recording(void);

#ifdef __cplusplus
}
#endif
