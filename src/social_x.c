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

// Body collector for esp_http_client. Allocates as needed; caller frees `*body`.
typedef struct { char *body; size_t len; size_t cap; int status; } http_collect_t;

static esp_err_t http_evt_collect(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    http_collect_t *c = (http_collect_t *)evt->user_data;
    if (!c) return ESP_OK;
    size_t need = c->len + evt->data_len + 1;
    if (need > c->cap) {
        size_t new_cap = c->cap ? c->cap * 2 : 1024;
        while (new_cap < need) new_cap *= 2;
        char *p = realloc(c->body, new_cap);
        if (!p) return ESP_FAIL;
        c->body = p; c->cap = new_cap;
    }
    memcpy(c->body + c->len, evt->data, evt->data_len);
    c->len += evt->data_len;
    c->body[c->len] = 0;
    return ESP_OK;
}

// POST a body, optionally with a Bearer token header. On return, `*out_body`
// holds the response (caller frees), `*out_status` the HTTP status. Body
// content_type is optional (NULL → "application/x-www-form-urlencoded").
static bool http_post_with_auth(const char *url, const char *body,
                                const char *content_type, const char *bearer,
                                char **out_body, int *out_status) {
    http_collect_t c = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_evt_collect,
        .user_data = &c,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    if (content_type)
        esp_http_client_set_header(h, "Content-Type", content_type);
    else
        esp_http_client_set_header(h, "Content-Type",
                                   "application/x-www-form-urlencoded");
    if (bearer) {
        char hdr[600];
        snprintf(hdr, sizeof(hdr), "Bearer %s", bearer);
        esp_http_client_set_header(h, "Authorization", hdr);
    }
    if (body)
        esp_http_client_set_post_field(h, body, strlen(body));
    esp_err_t rc = esp_http_client_perform(h);
    int st = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (rc != ESP_OK) {
        free(c.body);
        return false;
    }
    if (out_status) *out_status = st;
    if (out_body)   *out_body   = c.body;     // caller frees
    else            free(c.body);
    return true;
}

// GET helper for /2/users/me (just need a small response).
static bool http_get_with_auth(const char *url, const char *bearer,
                               char **out_body, int *out_status) {
    http_collect_t c = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_evt_collect,
        .user_data = &c,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    if (bearer) {
        char hdr[600];
        snprintf(hdr, sizeof(hdr), "Bearer %s", bearer);
        esp_http_client_set_header(h, "Authorization", hdr);
    }
    esp_err_t rc = esp_http_client_perform(h);
    int st = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (rc != ESP_OK) { free(c.body); return false; }
    if (out_status) *out_status = st;
    if (out_body)   *out_body   = c.body;
    else            free(c.body);
    return true;
}

// --- small JSON helpers (used by finish/refresh/post) ----------------------
static void cp_err(char *out, size_t cap, const char *msg) {
    if (out && cap) { strlcpy(out, msg, cap); }
}

// Reads "field" from a top-level JSON object. Returns a malloc'd copy
// (caller frees) or NULL.
static char *json_dup_string(cJSON *root, const char *field) {
    if (!root) return NULL;
    cJSON *v = cJSON_GetObjectItem(root, field);
    if (!cJSON_IsString(v) || !v->valuestring) return NULL;
    return strdup(v->valuestring);
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
    if (!s_pairing) {
        cp_err(out_err, out_err_cap, "no_pairing");
        return false;
    }
    if (!code || !code[0]) {
        cp_err(out_err, out_err_cap, "invalid_code");
        return false;
    }

    char redir_enc[256], client_enc[128], code_enc[256];
    url_encode(DAEMON_X_REDIRECT_URI, redir_enc,  sizeof(redir_enc));
    url_encode(DAEMON_X_CLIENT_ID,    client_enc, sizeof(client_enc));
    url_encode(code,                  code_enc,   sizeof(code_enc));

    char *body = malloc(1024);
    if (!body) { cp_err(out_err, out_err_cap, "oom"); return false; }
    snprintf(body, 1024,
        "grant_type=authorization_code"
        "&code=%s"
        "&redirect_uri=%s"
        "&client_id=%s"
        "&code_verifier=%s",
        code_enc, redir_enc, client_enc, s_verifier_b64);

    char *resp = NULL; int status = 0;
    bool ok = http_post_with_auth("https://api.x.com/2/oauth2/token",
                                  body, NULL, NULL, &resp, &status);
    free(body);

    if (!ok) {
        cp_err(out_err, out_err_cap, "network");
        free(resp);
        return false;
    }
    if (status != 200 || !resp) {
        ESP_LOGW(TAG, "token exchange failed: status=%d body=%s",
                 status, resp ? resp : "(null)");
        cp_err(out_err, out_err_cap, status == 400 ? "denied" : "network");
        free(resp);
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) { cp_err(out_err, out_err_cap, "parse"); return false; }

    char *access  = json_dup_string(root, "access_token");
    char *refresh = json_dup_string(root, "refresh_token");
    cJSON *exp    = cJSON_GetObjectItem(root, "expires_in");
    int    exp_s  = cJSON_IsNumber(exp) ? exp->valueint : 7200;
    cJSON_Delete(root);

    if (!access || !refresh) {
        cp_err(out_err, out_err_cap, "parse");
        free(access); free(refresh);
        return false;
    }

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    devcfg_set_x_tokens(access, refresh, now_s + (uint32_t)exp_s);

    char *me = NULL; int me_status = 0;
    if (http_get_with_auth("https://api.x.com/2/users/me", access,
                           &me, &me_status) && me_status == 200 && me) {
        cJSON *mr = cJSON_Parse(me);
        if (mr) {
            cJSON *data = cJSON_GetObjectItem(mr, "data");
            if (data) {
                char *u = json_dup_string(data, "username");
                if (u) { devcfg_set_x_handle(u); free(u); }
            }
            cJSON_Delete(mr);
        }
    }
    free(me);
    free(access); free(refresh);

    memset(s_verifier_raw, 0, sizeof(s_verifier_raw));
    memset(s_verifier_b64, 0, sizeof(s_verifier_b64));
    memset(s_state,        0, sizeof(s_state));
    s_pairing = false;

    ESP_LOGI(TAG, "paired with X as @%s", devcfg_x_handle());
    return true;
}
void social_x_disconnect(void) {
    const char *tok = devcfg_x_access_token();
    if (tok && tok[0]) {
        char client_enc[128], tok_enc[600];
        url_encode(DAEMON_X_CLIENT_ID, client_enc, sizeof(client_enc));
        url_encode(tok,                tok_enc,    sizeof(tok_enc));
        char *body = malloc(1024);
        if (body) {
            snprintf(body, 1024,
                "token=%s&client_id=%s&token_type_hint=access_token",
                tok_enc, client_enc);
            char *resp = NULL; int st = 0;
            (void)http_post_with_auth("https://api.x.com/2/oauth2/revoke",
                                      body, NULL, NULL, &resp, &st);
            free(body); free(resp);
            ESP_LOGI(TAG, "revoke status=%d", st);
        }
    }
    devcfg_clear_x();
    ESP_LOGI(TAG, "disconnected");
}
// Make a JSON-escaped copy of `s` into `out` for use inside a JSON string.
// Handles \", \\, \n, \r, \t. Other control bytes get \uXXXX.
// cap is the size of the output buffer; result is always NUL-terminated.
// Returns false on truncation.
static bool json_escape(const char *s, char *out, size_t cap) {
    static const char H[] = "0123456789abcdef";
    size_t o = 0;
    if (cap == 0) return false;
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) goto trunc;
            out[o++] = '\\'; out[o++] = c;
        } else if (c == '\n') {
            if (o + 2 >= cap) goto trunc;
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            if (o + 2 >= cap) goto trunc;
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\t') {
            if (o + 2 >= cap) goto trunc;
            out[o++] = '\\'; out[o++] = 't';
        } else if (c < 0x20) {
            if (o + 6 >= cap) goto trunc;
            out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
            out[o++] = H[(c >> 4) & 0xF]; out[o++] = H[c & 0xF];
        } else {
            if (o + 1 >= cap) goto trunc;
            out[o++] = c;
        }
    }
    out[o] = 0;
    return true;
trunc:
    out[cap ? cap - 1 : 0] = 0;
    return false;
}

// Refresh the access token using the stored refresh token. Returns true on
// success and updates devcfg. On failure, returns false; caller decides how
// to surface it (typically: clear and force re-pair).
static bool refresh_access_token(void) {
    const char *refresh = devcfg_x_refresh_token();
    if (!refresh || !refresh[0]) return false;

    char client_enc[128], rt_enc[600];
    url_encode(DAEMON_X_CLIENT_ID, client_enc, sizeof(client_enc));
    url_encode(refresh,            rt_enc,     sizeof(rt_enc));

    char *body = malloc(1024);
    if (!body) return false;
    snprintf(body, 1024,
        "grant_type=refresh_token"
        "&refresh_token=%s"
        "&client_id=%s",
        rt_enc, client_enc);

    char *resp = NULL; int st = 0;
    bool ok = http_post_with_auth("https://api.x.com/2/oauth2/token",
                                  body, NULL, NULL, &resp, &st);
    free(body);
    if (!ok || st != 200 || !resp) {
        free(resp);
        return false;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return false;
    char *access      = json_dup_string(root, "access_token");
    char *new_refresh = json_dup_string(root, "refresh_token");
    cJSON *exp        = cJSON_GetObjectItem(root, "expires_in");
    int    exp_s      = cJSON_IsNumber(exp) ? exp->valueint : 7200;
    cJSON_Delete(root);

    if (!access || !new_refresh) {
        free(access); free(new_refresh);
        return false;
    }
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    devcfg_set_x_tokens(access, new_refresh, now_s + (uint32_t)exp_s);
    free(access); free(new_refresh);
    ESP_LOGI(TAG, "access token refreshed");
    return true;
}

bool social_x_post(const char *text,
                   char *out_url, size_t out_url_cap,
                   char *out_err, size_t out_err_cap) {
    if (out_url && out_url_cap) out_url[0] = 0;
    if (out_err && out_err_cap) out_err[0] = 0;

    if (!text || !text[0]) {
        cp_err(out_err, out_err_cap, "empty");
        return false;
    }
    size_t tlen = strlen(text);
    if (tlen > 280) {
        cp_err(out_err, out_err_cap, "too_long");
        return false;
    }
    if (!devcfg_x_connected()) {
        cp_err(out_err, out_err_cap, "auth");
        return false;
    }

    char escaped[1024];
    if (!json_escape(text, escaped, sizeof(escaped))) {
        cp_err(out_err, out_err_cap, "too_long");
        return false;
    }
    char body[1100];
    int n = snprintf(body, sizeof(body), "{\"text\":\"%s\"}", escaped);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        cp_err(out_err, out_err_cap, "too_long");
        return false;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        const char *tok = devcfg_x_access_token();
        char *resp = NULL; int st = 0;
        bool ok = http_post_with_auth("https://api.x.com/2/tweets",
                                      body, "application/json", tok,
                                      &resp, &st);
        if (!ok) {
            cp_err(out_err, out_err_cap, "network");
            free(resp);
            return false;
        }
        if (st == 401 && attempt == 0) {
            free(resp);
            if (!refresh_access_token()) {
                devcfg_clear_x();
                cp_err(out_err, out_err_cap, "auth");
                return false;
            }
            continue;
        }
        if (st == 429) {
            cp_err(out_err, out_err_cap, "rate_limited");
            free(resp);
            return false;
        }
        if (st < 200 || st >= 300 || !resp) {
            ESP_LOGW(TAG, "post failed status=%d body=%s", st,
                     resp ? resp : "(null)");
            cp_err(out_err, out_err_cap, "network");
            free(resp);
            return false;
        }

        cJSON *root = cJSON_Parse(resp);
        free(resp);
        char *id = NULL;
        if (root) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            if (data) id = json_dup_string(data, "id");
            cJSON_Delete(root);
        }
        if (!id) {
            cp_err(out_err, out_err_cap, "parse");
            return false;
        }
        if (out_url && out_url_cap) {
            snprintf(out_url, out_url_cap,
                     "https://x.com/%s/status/%s",
                     devcfg_x_handle()[0] ? devcfg_x_handle() : "i", id);
        }
        free(id);
        return true;
    }
    cp_err(out_err, out_err_cap, "auth");
    return false;
}
