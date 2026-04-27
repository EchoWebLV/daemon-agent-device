// ---------------------------------------------------------------------------
//  PIN-gated seed storage. Phase 2b software foundation.
//
//  The wallet's signing seed is sealed in NVS encrypted with a key derived
//  from the user's PIN (PBKDF2-HMAC-SHA256, 100k iterations, per-device
//  salt). On boot, the firmware asks for the PIN, derives the key, and
//  unseals the seed into RAM. Wrong PINs increment a counter; after
//  PIN_MAX_ATTEMPTS failures the sealed blob + salt + counter are wiped
//  and the device must be re-provisioned from a fresh seed.
//
//  Argon2id is the long-term KDF target (better against GPU/ASIC brute
//  force) but PBKDF2 is in mbedtls today, so we ship with that and swap
//  the single KDF call once Argon2 is vendored. The on-disk layout is
//  KDF-agnostic (just the salt + the sealed blob), so an Argon2 cutover
//  is forward-compatible.
//
//  This module sits in the "zero chip risk" zone — no eFuse burns. Flash
//  encryption + Secure Boot v2 are the additive hardening layer (future
//  session, with backup hardware) that protect ciphertext on the flash
//  chip itself; the PIN gate works without them.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wipe trigger: after this many consecutive wrong PIN attempts the sealed
// seed + salt + counter are erased.
#define PIN_MAX_ATTEMPTS 5

// Hard caps on what we can seal. The wallet today supports 32-byte
// (pubkey-only) or 64-byte (Phantom-style seed||pubkey) keys; 96 leaves
// headroom for future formats.
#define PIN_MAX_SEED_LEN 96

// PIN length sanity bounds. UI may further constrain.
#define PIN_MIN_LEN 4
#define PIN_MAX_LEN 32

typedef enum {
    PIN_OK            = 0,
    PIN_ERR_NOT_SET   = -1,  // no sealed seed exists
    PIN_ERR_BAD_PIN   = -2,  // KDF + AEAD tag mismatch
    PIN_ERR_WIPED     = -3,  // attempts exhausted; seed gone
    PIN_ERR_NVS       = -4,  // NVS read/write failure
    PIN_ERR_INTERNAL  = -5,  // crypto/library failure
    PIN_ERR_BAD_ARG   = -6,  // caller passed invalid input
} pin_status_t;

// Open the NVS handle. Safe to call multiple times. Returns PIN_OK or PIN_ERR_NVS.
pin_status_t pin_init(void);

// True if a sealed seed already exists in NVS.
bool pin_is_set(void);

// Number of wrong-PIN attempts left before auto-wipe. PIN_MAX_ATTEMPTS when no
// PIN is set (or after a successful unlock); 0 immediately before the next
// failure triggers a wipe.
int pin_attempts_remaining(void);

// First-time setup: encrypt `seed` with a key derived from `pin` and stash
// in NVS along with a fresh random salt. Existing sealed data (if any) is
// overwritten.
pin_status_t pin_setup(const char *pin, const uint8_t *seed, size_t seed_len);

// Try to unseal with `pin`. On success, writes the seed bytes into
// `out_seed` (must be at least PIN_MAX_SEED_LEN), returns the actual length
// in `*out_len`, and resets the failure counter.
//
// On wrong PIN: PIN_ERR_BAD_PIN, attempts counter incremented. If that
// pushes the counter past PIN_MAX_ATTEMPTS, the sealed blob is wiped and
// PIN_ERR_WIPED is returned.
pin_status_t pin_unlock(const char *pin,
                        uint8_t *out_seed, size_t out_cap, size_t *out_len);

// Erase the sealed seed + salt + counter from NVS. Used by setup overwrites
// and by the auto-wipe path. Idempotent.
pin_status_t pin_wipe(void);

#ifdef __cplusplus
}
#endif
