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

// --- base64url (RFC 4648 §5, no padding) -----------------------------------
// mbedTLS gives us standard base64; we post-process to URL-safe and strip '='.
static bool b64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)out, out_cap, &olen, in, in_len);
    if (rc != 0) return false;
    // Strip padding, swap +/ for -_
    char *p = out;
    while (olen > 0 && p[olen - 1] == '=') { olen--; p[olen] = 0; }
    for (size_t i = 0; i < olen; i++) {
        if      (p[i] == '+') p[i] = '-';
        else if (p[i] == '/') p[i] = '_';
    }
    return true;
}

// --- PKCE pair generation --------------------------------------------------
// verifier:  64 random bytes, base64url-encoded → ~86 chars
// challenge: SHA256(verifier_b64), base64url-encoded → 43 chars
static bool make_pkce_pair(char *verifier_out, size_t verifier_cap,
                           char *challenge_out, size_t challenge_cap) {
    uint8_t raw[VERIFIER_LEN];
    esp_fill_random(raw, sizeof(raw));
    if (!b64url_encode(raw, sizeof(raw), verifier_out, verifier_cap)) return false;

    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *)verifier_out, strlen(verifier_out),
                   digest, 0);
    if (!b64url_encode(digest, sizeof(digest), challenge_out, challenge_cap)) return false;
    return true;
}

// --- random hex state ------------------------------------------------------
static void make_state_hex(char *out, size_t cap) {
    uint8_t b[8];
    esp_fill_random(b, sizeof(b));
    static const char H[] = "0123456789abcdef";
    size_t need = 2 * sizeof(b) + 1;
    if (cap < need) { if (cap) out[0] = 0; return; }
    for (size_t i = 0; i < sizeof(b); i++) {
        out[i * 2 + 0] = H[(b[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[(b[i] >> 0) & 0xF];
    }
    out[2 * sizeof(b)] = 0;
}

// URL-encode helper — narrow, only what we put into auth URL params.
static void url_encode(const char *in, char *out, size_t cap) {
    static const char H[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                           c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) { out[o++] = c; }
        else {
            out[o++] = '%';
            out[o++] = H[(c >> 4) & 0xF];
            out[o++] = H[(c >> 0) & 0xF];
        }
    }
    if (o < cap) out[o] = 0;
}

bool social_x_begin(char *out_url, size_t out_url_cap) {
    char challenge[64];
    if (!make_pkce_pair(s_verifier_b64, sizeof(s_verifier_b64),
                        challenge,      sizeof(challenge))) {
        ESP_LOGE(TAG, "PKCE generation failed");
        return false;
    }
    make_state_hex(s_state, sizeof(s_state));
    s_pairing = true;

    char redir_enc[256];
    char client_enc[128];
    url_encode(DAEMON_X_REDIRECT_URI, redir_enc,  sizeof(redir_enc));
    url_encode(DAEMON_X_CLIENT_ID,    client_enc, sizeof(client_enc));
    char scope_enc[128];
    url_encode("tweet.read tweet.write users.read offline.access",
               scope_enc, sizeof(scope_enc));

    int n = snprintf(out_url, out_url_cap,
        "https://x.com/i/oauth2/authorize"
        "?response_type=code"
        "&client_id=%s"
        "&redirect_uri=%s"
        "&scope=%s"
        "&code_challenge=%s"
        "&code_challenge_method=S256"
        "&state=%s",
        client_enc, redir_enc, scope_enc, challenge, s_state);
    if (n < 0 || (size_t)n >= out_url_cap) {
        ESP_LOGE(TAG, "auth URL truncated");
        return false;
    }
    ESP_LOGI(TAG, "pairing started (verifier kept in RAM)");
    return true;
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
