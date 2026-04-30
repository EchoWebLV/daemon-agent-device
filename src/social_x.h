// ---------------------------------------------------------------------------
//  X (Twitter) connector for Daemon. Pure on-device OAuth 2.0 PKCE — the
//  device generates its own verifier, exchanges the user-pasted auth code
//  for tokens, refreshes on expiry, and posts to api.x.com directly.
//
//  All persistent state lives in NVS via devcfg (see devcfg_x_*).
//  Pairing-time verifier lives only in RAM in this module.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pairing — step 1: caller passes a buffer; on success it's filled with the
// X authorize URL the user should open in a new tab. RAM verifier slot is
// allocated here; any prior in-flight pairing is overwritten.
// Returns true on success.
bool social_x_begin(char *out_url, size_t out_url_cap);

// Pairing — step 2: caller passes the auth code the user copied from the
// bounce page. Exchanges it for tokens (using the in-RAM verifier), then
// fetches the user handle and persists everything to NVS.
//
// Returns true on success. On failure, fills `out_err` with a short
// human-readable reason ("network", "denied", "invalid_code", "no_pairing", ...).
bool social_x_finish(const char *code, char *out_err, size_t out_err_cap);

// Tear down: best-effort revoke at X, always clear NVS.
void social_x_disconnect(void);

// Posting — synchronous. Opens the on-device approval modal, blocks until
// the user approves / cancels, then on approval POSTs to /2/tweets.
//
// On success, fills `out_url` with the tweet URL.
// On any non-success, fills `out_err` with a short reason
// ("user_cancelled", "network", "rate_limited", "auth", "too_long", ...).
//
// Either out_url or out_err is filled; the other will be empty.
bool social_x_post(const char *text,
                   char *out_url, size_t out_url_cap,
                   char *out_err, size_t out_err_cap);

#ifdef __cplusplus
}
#endif
