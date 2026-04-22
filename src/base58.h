// ---------------------------------------------------------------------------
//  Minimal base58 codec — just enough for Solana key handling.
//  The alphabet matches Bitcoin/Solana base58 (no 0, O, I, l).
//
//  API is pure C and caller-allocates: the caller sizes the output buffer
//  and we report how many bytes we actually wrote (or -1 on failure).
//  Output buffers should be sized with the BASE58_*_MAX helpers below;
//  they're worst-case and non-tight so a little overshoot is fine.
// ---------------------------------------------------------------------------
#pragma once
#include <stddef.h>
#include <stdint.h>

// Worst-case output lengths. log(256)/log(58) ≈ 1.365 and log(58)/log(256)
// ≈ 0.733 — same constants as the Bitcoin reference.
#define BASE58_ENCODE_MAX_LEN(in_bytes)  (((size_t)(in_bytes) * 138) / 100 + 2)
#define BASE58_DECODE_MAX_LEN(in_chars)  (((size_t)(in_chars) * 733) / 1000 + 2)

// Decode a NUL-terminated base58 string into `out`. Returns the number of
// bytes written, or -1 on malformed input / buffer too small.
int base58_decode(const char *in, uint8_t *out, size_t out_cap);

// Encode `len` bytes into `out` (NUL-terminated). Returns the number of
// characters written (not counting the NUL), or -1 on buffer too small.
int base58_encode(const uint8_t *bytes, size_t len, char *out, size_t out_cap);
