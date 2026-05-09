// ---------------------------------------------------------------------------
//  mic.c — see mic.h.
//
//  Thin shim over wake.c. The codec and the capture buffer used to live
//  here, opened on PTT-down and closed on PTT-up. With the wake-word
//  engine in play (always-listening AFE pipeline, see src/wake.c) the
//  codec needs to stay open continuously, so wake.c took ownership of
//  the codec lifecycle and the PSRAM capture buffer.
//
//  This file keeps the mic.h API intact so creature_screen.c's PTT
//  handlers don't have to change — mic_record_start/stop just enable
//  and drain wake.c's capture diversion. AFE keeps detecting the wake
//  word during a PTT press; wake.c's mode interlock prevents
//  double-capture.
// ---------------------------------------------------------------------------
#include "mic.h"
#include "wake.h"

#include "esp_heap_caps.h"

esp_err_t mic_init(void) {
    // wake_init (called from app_main) owns the codec bring-up and the
    // capture buffer. mic_init becomes a no-op so existing call sites
    // can keep their "mic_init failed; PTT disabled" log and not have
    // to know about wake.c at all.
    return ESP_OK;
}

esp_err_t mic_record_start(void) {
    return wake_capture_start();
}

esp_err_t mic_record_stop(int16_t **out_pcm, size_t *out_frames) {
    return wake_capture_stop(out_pcm, out_frames);
}

void mic_buffer_free(int16_t *pcm) {
    if (pcm) heap_caps_free(pcm);
}

bool mic_is_recording(void) {
    return wake_capture_is_active();
}
