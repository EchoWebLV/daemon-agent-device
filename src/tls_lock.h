// ---------------------------------------------------------------------------
//  tls_lock — global mutex serialising all outgoing HTTPS calls.
//
//  ESP32-S3 has ~320 KB of internal RAM, and mbedtls AES contexts have to
//  live there (DMA-capable / cache-disable-safe memory). Each concurrent
//  TLS handshake allocates its own AES context. With wallet refresh, price
//  refresh, scheduler fires, STT, AI, TTS, and the device's web server
//  all running in parallel, two TLS handshakes overlap reliably and
//  mbedtls crashes the second one with `esp-aes: Failed to allocate memory`.
//  The user sees "I didn't catch that" or "I couldn't complete the USDC
//  payment" or partial/silent TTS.
//
//  The fix: a single FreeRTOS mutex every TLS-using path takes before
//  opening the connection and releases after closing. Only one TLS
//  handshake exists at a time. AES alloc never fails because there's
//  only ever one AES context. Background refreshes (wallet, price)
//  delay slightly when foreground PTT is in flight; that's correct
//  prioritisation — the user is talking, not staring at the SOL ticker.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the mutex. Safe to call more than once. MUST be called before
// any TLS-using subsystem (wallet, price, ai, voice, stt, x402, social_x,
// scheduler, payapi). app_main does this very early.
void tls_lock_init(void);

// Block until we hold the lock or `timeout_ms` elapses (UINT32_MAX = wait
// forever). Returns true if the lock is now held by the calling task.
//
// Pair every successful take with exactly one give. Failure to acquire
// the lock means the caller MUST NOT proceed with the TLS call — they
// should return an error to their caller and try again later.
bool tls_lock_take(uint32_t timeout_ms);

// Release the lock. Must be called from the same task that took it.
void tls_lock_give(void);

#ifdef __cplusplus
}
#endif
