// ---------------------------------------------------------------------------
//  stt.h — speech-to-text via the same x402 gateway the chat path uses.
//
//  POSTs a 16 kHz/16-bit/mono PCM buffer (wrapped as a WAV) to
//  https://sol.blockrun.ai/api/v1/audio/transcriptions and returns the
//  transcript. Pays per-call via the existing x402 facility (same path as
//  ai.c's chat round-trip) so no user-supplied API key is needed.
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
