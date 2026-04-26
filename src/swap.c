// ---------------------------------------------------------------------------
//  swap.c — see swap.h.
//
//  Pipeline (one swap):
//    1. resolve symbols → mints + decimals
//    2. balance check against wallet snapshot
//    3. Jupiter /quote → outAmount + min_out + verbatim quote-response JSON
//    4. open approval modal, block on confirm/cancel
//    5. Jupiter /swap (sends quote-response back) → unsigned v0 tx
//    6. parse v0 wire format, locate our signature slot, sign + splice
//    7. RPC sendTransaction
//    8. RPC getSignatureStatuses, poll up to 30s
//    9. wallet_request_refresh() → creature speaks via incoming-payment cb
// ---------------------------------------------------------------------------
#include "swap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "wallet.h"
#include "wifi_sta.h"

static const char *TAG = "swap";
static double swap_pow10(uint8_t n);

// Jupiter response sizes. Quote response is a few KB of routing detail;
// 6 KB is comfortable headroom. Swap response inflates with multi-hop
// + ALT references — 12 KB is the documented ceiling for v6 single-tx
// swaps. We reject anything larger as SWAP_ERR_BUILD.
#define JUP_QUOTE_RSP_CAP 6144
#define JUP_SWAP_RSP_CAP  12288

typedef struct { char *buf; size_t cap; size_t len; } http_body_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    http_body_t *r = (http_body_t *)evt->user_data;
    if (!r || !r->buf || r->cap == 0) return ESP_OK;
    size_t room = r->cap - 1 - r->len;
    if (room == 0) return ESP_OK;
    size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    memcpy(r->buf + r->len, evt->data, take);
    r->len += take;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

// One-shot HTTPS GET. Returns true iff status == 200 and the body fit.
static bool https_get(const char *url, char *out, size_t out_cap) {
    if (!wifi_sta_is_connected() || out_cap == 0) return false;
    out[0] = '\0';

    http_body_t rb = { .buf = out, .cap = out_cap, .len = 0 };
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = on_http_event,
        .user_data         = &rb,
        .timeout_ms        = 15000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    esp_err_t err  = esp_http_client_perform(h);
    int       code = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "GET %s: %s", url, esp_err_to_name(err)); return false; }
    if (code != 200)   { ESP_LOGW(TAG, "GET %s: HTTP %d", url, code);             return false; }
    return true;
}

// One-shot HTTPS POST with JSON body.
static bool __attribute__((unused)) https_post_json(const char *url, const char *body,
                             char *out, size_t out_cap) {
    if (!wifi_sta_is_connected() || out_cap == 0) return false;
    out[0] = '\0';

    http_body_t rb = { .buf = out, .cap = out_cap, .len = 0 };
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = on_http_event,
        .user_data         = &rb,
        .timeout_ms        = 15000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, body, (int)strlen(body));
    esp_err_t err  = esp_http_client_perform(h);
    int       code = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "POST %s: %s", url, esp_err_to_name(err)); return false; }
    if (code != 200)   { ESP_LOGW(TAG, "POST %s: HTTP %d", url, code);             return false; }
    return true;
}

// Jupiter accepts wrapped-SOL mint as the sentinel for native SOL whenever
// the request includes wrapAndUnwrapSol=true. We always do, so we always
// pass this for "SOL".
static const char *NATIVE_SOL_MINT = "So11111111111111111111111111111111111111112";
static const char *USDC_MINT       = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

typedef struct {
    char    mint[45];      // base58 mint, NUL-terminated
    char    sym[12];        // canonical symbol ("SOL", "USDC", or wallet ticker)
    uint8_t decimals;
} swap_token_t;

// Resolve a user-supplied symbol to mint + decimals. Case-insensitive.
// "SOL" / "USDC" are hard-coded; anything else is matched against the
// wallet's most recent SPL holdings (first match wins on duplicate symbols).
// Returns false if not resolvable.
static bool resolve_token(const char *sym, swap_token_t *out) {
    // Reject empty sym: a wallet entry that hasn't been hydrated yet has
    // symbol[0] == '\0', and an empty input would falsely match it.
    if (!sym || !*sym || !out) return false;
    memset(out, 0, sizeof(*out));

    if (strcasecmp(sym, "SOL") == 0) {
        strlcpy(out->mint, NATIVE_SOL_MINT, sizeof(out->mint));
        strlcpy(out->sym,  "SOL",            sizeof(out->sym));
        out->decimals = 9;
        return true;
    }
    if (strcasecmp(sym, "USDC") == 0) {
        strlcpy(out->mint, USDC_MINT, sizeof(out->mint));
        strlcpy(out->sym,  "USDC",     sizeof(out->sym));
        out->decimals = 6;
        return true;
    }

    const token_holding_t *toks = NULL;
    size_t n = 0;
    wallet_tokens(&toks, &n);
    for (size_t i = 0; i < n; ++i) {
        if (strcasecmp(toks[i].symbol, sym) == 0) {
            strlcpy(out->mint, toks[i].mint,   sizeof(out->mint));
            strlcpy(out->sym,  toks[i].symbol, sizeof(out->sym));
            out->decimals = toks[i].decimals;
            return true;
        }
    }
    return false;
}

// Returns true if the wallet snapshot says we hold at least `amount_ui`
// of the symbol resolved by `t`. SPL holdings are matched by mint, not
// symbol — handles the "two tokens with the same ticker" case correctly.
static bool __attribute__((unused)) has_balance(const swap_token_t *t, double amount_ui) {
    if (!t || !isfinite(amount_ui) || amount_ui < 0.0) return false;
    if (strcmp(t->sym, "SOL") == 0) {
        return wallet_sol_balance() >= amount_ui;
    }
    if (strcmp(t->sym, "USDC") == 0) {
        return wallet_usdc_amount() >= amount_ui;
    }
    const token_holding_t *toks = NULL;
    size_t n = 0;
    wallet_tokens(&toks, &n);
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(toks[i].mint, t->mint) == 0) {
            return toks[i].amount >= amount_ui;
        }
    }
    return false;
}

// Slippage defaults: 50 bps for SOL/USDC, 100 bps for any pair involving
// an SPL. Caller-supplied values outside [10, 500] fall back to default.
static uint16_t __attribute__((unused)) default_slippage(const swap_token_t *a, const swap_token_t *b) {
    bool a_major = (strcmp(a->sym, "SOL") == 0 || strcmp(a->sym, "USDC") == 0);
    bool b_major = (strcmp(b->sym, "SOL") == 0 || strcmp(b->sym, "USDC") == 0);
    return (a_major && b_major) ? 50 : 100;
}

static uint16_t __attribute__((unused)) clamp_slippage(uint16_t requested, uint16_t fallback) {
    if (requested == 0)                        return fallback;
    if (requested < 10 || requested > 500)     return fallback;
    return requested;
}

// Jupiter v6 /quote → fills out the per-swap numbers AND captures the
// verbatim quoteResponse JSON, which we hand back to /swap unchanged.
//
// `quote_json_out` is a heap allocation owned by the caller; freed via
// free(). NULL on failure.
typedef struct {
    uint64_t in_atomic;
    uint64_t out_atomic;
    uint64_t min_out_atomic;   // otherAmountThreshold
    char    *quote_json;       // heap, free()
} jup_quote_t;

static bool __attribute__((unused)) jup_get_quote(const swap_token_t *from, const swap_token_t *to,
                          double amount_ui, uint16_t slippage_bps,
                          jup_quote_t *out) {
    memset(out, 0, sizeof(*out));
    uint64_t atomic = (uint64_t)(amount_ui * swap_pow10(from->decimals) + 0.5);
    if (atomic == 0) return false;

    char url[512];
    snprintf(url, sizeof(url),
             "https://quote-api.jup.ag/v6/quote"
             "?inputMint=%s&outputMint=%s&amount=%llu&slippageBps=%u"
             "&onlyDirectRoutes=false&restrictIntermediateTokens=true",
             from->mint, to->mint,
             (unsigned long long)atomic, (unsigned)slippage_bps);

    char *rsp = heap_caps_malloc(JUP_QUOTE_RSP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rsp) return false;
    if (!https_get(url, rsp, JUP_QUOTE_RSP_CAP)) { free(rsp); return false; }

    cJSON *root = cJSON_Parse(rsp);
    if (!root) { free(rsp); ESP_LOGW(TAG, "quote: parse fail"); return false; }

    const cJSON *out_amt = cJSON_GetObjectItem(root, "outAmount");
    const cJSON *thr     = cJSON_GetObjectItem(root, "otherAmountThreshold");
    if (!cJSON_IsString(out_amt) || !cJSON_IsString(thr)) {
        cJSON_Delete(root); free(rsp);
        ESP_LOGW(TAG, "quote: missing fields");
        return false;
    }

    out->in_atomic     = atomic;
    out->out_atomic    = strtoull(out_amt->valuestring, NULL, 10);
    out->min_out_atomic= strtoull(thr->valuestring, NULL, 10);
    out->quote_json    = rsp;          // hand off ownership; do NOT free here
    cJSON_Delete(root);
    return true;
}

// Tiny helper — raises 10 to the power n without relying on GNU pow10().
static double swap_pow10(uint8_t n) {
    double v = 1.0;
    for (uint8_t i = 0; i < n; ++i) v *= 10.0;
    return v;
}

bool swap_resolve_for_test(const char *sym, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    swap_token_t t;
    if (!resolve_token(sym, &t)) return false;
    snprintf(out, cap, "%s:%u", t.mint, (unsigned)t.decimals);
    return true;
}

bool swap_request(const char *from_sym,
                  const char *to_sym,
                  double      amount_ui,
                  uint16_t    slippage_bps,
                  swap_result_t *out) {
    (void)from_sym; (void)to_sym; (void)amount_ui; (void)slippage_bps;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->status = SWAP_ERR_QUOTE;
    strlcpy(out->error_msg, "not_implemented", sizeof(out->error_msg));
    ESP_LOGW(TAG, "swap_request stub — not implemented yet");
    return false;
}
