// ---------------------------------------------------------------------------
//  X connector — see social_x.h.
// ---------------------------------------------------------------------------
#include "social_x.h"
#include "devcfg.h"
#include "secrets.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#ifndef DAEMON_X_CLIENT_ID
#define DAEMON_X_CLIENT_ID "PLACEHOLDER_NOT_CONFIGURED"
#endif
#ifndef DAEMON_X_REDIRECT_URI
#define DAEMON_X_REDIRECT_URI "https://example.com/x-callback"
#endif

static const char *TAG = "social_x";

// Pairing-time RAM slots. Cleared on success or on a fresh begin() call.
#define VERIFIER_LEN  64                 // raw bytes; b64url-encoded form is ~86 chars
static uint8_t s_verifier_raw[VERIFIER_LEN];
static char    s_verifier_b64[128];      // base64url-encoded verifier
static char    s_state[24];              // 8-byte hex state for CSRF
static bool    s_pairing = false;

// stub bodies — implemented in later tasks
bool social_x_begin(char *out_url, size_t out_url_cap) {
    (void)out_url; (void)out_url_cap;
    return false;
}
bool social_x_finish(const char *code, char *out_err, size_t out_err_cap) {
    (void)code; (void)out_err; (void)out_err_cap;
    return false;
}
void social_x_disconnect(void) {
    devcfg_clear_x();
}
bool social_x_post(const char *text,
                   char *out_url, size_t out_url_cap,
                   char *out_err, size_t out_err_cap) {
    (void)text; (void)out_url; (void)out_url_cap;
    (void)out_err; (void)out_err_cap;
    return false;
}
