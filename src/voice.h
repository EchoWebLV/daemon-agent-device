// ---------------------------------------------------------------------------
//  Voice playback — Daemon's TTS path.
//
//  Pipes ElevenLabs' streaming PCM output directly into the ESP32-S3's I2S
//  peripheral (TX-only) driving a PCM5101 codec on the Waveshare 2.8" board.
//
//  Why raw PCM instead of MP3: we don't ship an MP3 decoder in this ESP-IDF
//  build. ElevenLabs' `pcm_22050` output gives us 22 050 Hz / 16-bit / mono
//  directly on the wire, so we can stream the HTTP response body straight
//  into the I2S DMA. Latency is effectively "first bytes over TLS" — no
//  decoder warm-up, no filesystem round-trip. The trade-off is ~44 KB/s
//  downstream vs ~4 KB/s for mp3_22050_32; on any half-decent WiFi link
//  the extra bandwidth is a rounding error next to 75-200 ms of model TTFB.
//
//  Threading:
//    • voice_begin() installs the I2S driver on the caller's thread and
//      spawns a dedicated audio task that owns i2s_channel_write().
//    • voice_speak() enqueues a request and returns immediately.
//    • voice_stop() sets an abort flag; the task drops the in-flight body
//      and goes back to idle on the next event-loop pump.
//
//  Pin map (PCM5101 on the Waveshare 2.8"):
//      BCLK = GPIO48
//      LRCK = GPIO38
//      DOUT = GPIO47
// ---------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the shared esp_codec_dev I2S data_if (TX+RX), created during
// voice_begin(). mic.c uses this to attach the ES7210 input codec to the
// same I2S port without trying to re-initialize the channels. Returns
// NULL before voice_begin() has run.
const audio_codec_data_if_t *voice_audio_data_if(void);

// Synchronously play back a buffer of raw mono int16 PCM at the codec's
// sample rate (22050 Hz). Used for the mic-loopback diagnostic — verifies
// that captured audio is real speech before we trust STT to handle it.
// Blocks for the duration of the audio. Safe to call from any task that
// can hold up for ~1 second; not safe from the LVGL UI thread.
bool voice_play_pcm(const int16_t *pcm, size_t frames);

// Install I2S + spawn the audio task. Plays a short 3-note boot beep so a
// dead codec / bad wiring is obvious before the first TTS attempt.
// Returns false if the I2S driver or the task couldn't come up.
bool voice_begin(void);

// Kicks a TTS round-trip on the audio task. Returns false immediately on
// precondition failures (no Wi-Fi, missing ElevenLabs key, empty text,
// voice not initialised). The request is asynchronous — voice_is_speaking()
// will flip to true once bytes start flowing out of I2S.
bool voice_speak(const char *text);

// True while audio is actively streaming or the HTTP POST is in flight.
bool voice_is_speaking(void);

// Abort the current utterance. Safe to call from any thread. No-op when
// nothing is playing.
void voice_stop(void);

// Software volume. 0..21, applied as a linear multiplier on the PCM samples
// before they go to I2S. 0 = mute, 21 = unity gain.
void voice_set_volume(uint8_t v);

// One-shot HTTPS probe — POSTs a tiny utterance to ElevenLabs and logs
// the status + first 64 bytes of the body without playing anything. Useful
// to verify the API key + TLS bundle in isolation. Returns true iff the
// server accepted the request with 2xx.
bool voice_diagnose(void);

#ifdef __cplusplus
}
#endif
