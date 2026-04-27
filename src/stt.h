// ---------------------------------------------------------------------------
//  stt.h — speech-to-text via OpenAI Whisper.
//
//  POSTs a 16 kHz/16-bit/mono PCM buffer (wrapped as a WAV) to
//  https://api.openai.com/v1/audio/transcriptions and returns the transcript.
//
//  Whisper is paid via OPENAI_API_KEY on the user's account — NOT via x402,
//  because the existing x402.c client only speaks application/json and the
//  audio endpoint requires multipart/form-data. When/if our facilitator
//  sprouts a multipart variant we'll route this through there too.
// ---------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Synchronously transcribe `pcm` (frames * sizeof(int16_t) bytes of
// 16 kHz mono PCM). Writes UTF-8 transcript into out_text (NUL-terminated,
// truncated to out_cap-1 chars on overflow). Returns true on success.
//
// Blocks the caller for 2-10 seconds typically. Don't call from the LVGL
// UI thread; spawn a FreeRTOS task.
bool stt_transcribe(const int16_t *pcm, size_t frames,
                    char *out_text, size_t out_cap);

#ifdef __cplusplus
}
#endif
