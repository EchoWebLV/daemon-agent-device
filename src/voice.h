// ---------------------------------------------------------------------------
//  Voice playback — Daemon's TTS path.
//
//  Streams ElevenLabs `pcm_16000` (16 kHz / 16-bit / mono) directly into the
//  BOX-3's ES8311 codec via the `esp-box-3` BSP. No MP3 decoder in the build
//  — raw PCM means audio starts the moment the first TLS bytes land, with
//  no decoder warm-up or filesystem round-trip. The bandwidth cost (~32 KB/s
//  vs ~4 KB/s for mp3_22050_32) is rounding error against typical 75–200 ms
//  model TTFB on Wi-Fi.
//
//  Threading:
//    • voice_begin() initialises the codec via the BSP and spawns the
//      audio task that owns esp_codec_dev_write().
//    • voice_speak() enqueues a request and returns immediately.
//    • voice_stop() sets an abort flag; the task drops the in-flight body
//      and goes back to idle on the next event-loop pump.
// ---------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the ES8311 codec via bsp_audio_codec_speaker_init() and start the
// audio task. Plays a short 3-note boot beep so a dead codec / bad wiring
// is obvious before the first TTS attempt. Returns false if the codec or
// task couldn't come up.
bool voice_begin(void);

// Single-shot TTS. Drains any queued chunks and plays this immediately
// (interrupt-replace semantics). Returns false immediately on precondition
// failures (no Wi-Fi, missing ElevenLabs key, empty text, voice not
// initialised). Asynchronous — voice_is_speaking() flips to true once
// bytes start flowing out of I2S.
bool voice_speak(const char *text);

// Append a TTS chunk to the back of the queue. Used by the streaming
// chat path so multiple sentence-bounded fragments play back-to-back as
// the LLM emits them. Returns false if the queue is full (chunk dropped).
bool voice_speak_chunk(const char *text);

// True while audio is actively streaming or the HTTP POST is in flight.
bool voice_is_speaking(void);

// Abort the current utterance. Safe to call from any thread. No-op when
// nothing is playing.
void voice_stop(void);

// Stop the current utterance AND drop any queued chunks. Use this when
// the user has actively cancelled (PTT interrupt) so a stale streaming
// task can't keep enqueueing audio after the cancel.
void voice_clear(void);

// Software volume. 0..21, applied as a linear multiplier on the PCM samples
// before they go to I2S. 0 = mute, 21 = unity gain.
void voice_set_volume(uint8_t v);

// Play a tiny acknowledgement tone (~80 ms). Used as immediate audible
// feedback when the user releases the push-to-talk button so the wait
// for STT + LLM doesn't feel like a dead device. Non-blocking; queues
// onto the audio task. No-op if voice isn't up or TTS is already playing.
void voice_chirp(void);

#ifdef __cplusplus
}
#endif
