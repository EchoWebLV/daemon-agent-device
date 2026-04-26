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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "ed25519.h"
#include "swap_screen.h"
#include "wallet.h"
#include "wifi_sta.h"

static const char *TAG = "swap";
static double swap_pow10(uint8_t n);
static int sign_v0_tx_in_place(const char *in_b64, char *out_b64, size_t out_cap);
static bool rpc_send_tx(const char *signed_tx_b64, char *txid_out, size_t cap);
static bool rpc_wait_for_confirm(const char *txid);

// Jupiter response sizes. Quote response is a few KB of routing detail;
// 6 KB is comfortable headroom. Swap response inflates with multi-hop
// + ALT references — 12 KB is the documented ceiling for v6 single-tx
// swaps. We reject anything larger as SWAP_ERR_BUILD.
#define JUP_QUOTE_RSP_CAP 6144
#define JUP_SWAP_RSP_CAP  12288

// v0 transaction buffer sizes. Jupiter swap wire txs are typically ~1.2 KB;
// 4 KB raw + 8 KB base64 covers everything observed in practice.
#define SWAP_TX_RAW_CAP   4096
#define SWAP_TX_B64_CAP   8192

// compact-u16 (Solana's "shortvec"): 1-3 bytes, low 7 bits of each byte are
// data, MSB indicates continuation. Returns false if the buffer is exhausted
// or all 3 bytes have the continuation bit set (malformed).
static bool compact_u16_read(const uint8_t *buf, size_t cap,
                             size_t *cursor, uint16_t *out) {
    uint16_t v = 0;
    for (int i = 0; i < 3; ++i) {
        if (*cursor >= cap) return false;
        uint8_t b = buf[(*cursor)++];
        v |= ((uint16_t)(b & 0x7F)) << (7 * i);
        if ((b & 0x80) == 0) {
            *out = v;
            return true;
        }
    }
    return false;
}

typedef struct { char *buf; size_t cap; size_t len; bool overflowed; } http_body_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    http_body_t *r = (http_body_t *)evt->user_data;
    if (!r || !r->buf || r->cap == 0) return ESP_OK;
    size_t room = r->cap - 1 - r->len;
    if (evt->data_len > 0 && (size_t)evt->data_len > room) r->overflowed = true;
    if (room == 0) return ESP_OK;
    size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    memcpy(r->buf + r->len, evt->data, take);
    r->len += take;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

// One-shot HTTPS GET. Returns true iff status == 200 and the entire body fit
// inside `out` (a truncated body is rejected — silent truncation would let
// cJSON parse a partial response and produce wrong numbers).
static bool https_get(const char *url, char *out, size_t out_cap) {
    if (!wifi_sta_is_connected() || out_cap == 0) return false;
    out[0] = '\0';

    http_body_t rb = { .buf = out, .cap = out_cap, .len = 0, .overflowed = false };
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
    if (err != ESP_OK)  { ESP_LOGW(TAG, "GET %s: %s", url, esp_err_to_name(err)); return false; }
    if (code != 200)    { ESP_LOGW(TAG, "GET %s: HTTP %d", url, code);             return false; }
    if (rb.overflowed)  { ESP_LOGW(TAG, "GET %s: body > %u bytes", url, (unsigned)out_cap); return false; }
    return true;
}

// One-shot HTTPS POST with JSON body. Truncated bodies are rejected (see
// `https_get` for rationale).
static bool https_post_json(const char *url, const char *body,
                             char *out, size_t out_cap) {
    if (!wifi_sta_is_connected() || out_cap == 0) return false;
    out[0] = '\0';

    http_body_t rb = { .buf = out, .cap = out_cap, .len = 0, .overflowed = false };
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
    if (err != ESP_OK)  { ESP_LOGW(TAG, "POST %s: %s", url, esp_err_to_name(err)); return false; }
    if (code != 200)    { ESP_LOGW(TAG, "POST %s: HTTP %d", url, code);             return false; }
    if (rb.overflowed)  { ESP_LOGW(TAG, "POST %s: body > %u bytes", url, (unsigned)out_cap); return false; }
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
static bool has_balance(const swap_token_t *t, double amount_ui) {
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
static uint16_t default_slippage(const swap_token_t *a, const swap_token_t *b) {
    bool a_major = (strcmp(a->sym, "SOL") == 0 || strcmp(a->sym, "USDC") == 0);
    bool b_major = (strcmp(b->sym, "SOL") == 0 || strcmp(b->sym, "USDC") == 0);
    return (a_major && b_major) ? 50 : 100;
}

static uint16_t clamp_slippage(uint16_t requested, uint16_t fallback) {
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

static bool jup_get_quote(const swap_token_t *from, const swap_token_t *to,
                          double amount_ui, uint16_t slippage_bps,
                          jup_quote_t *out) {
    memset(out, 0, sizeof(*out));
    if (!isfinite(amount_ui) || amount_ui < 0.0) return false;
    // Avoid double→uint64_t UB on overflow. 1e18 is well below UINT64_MAX
    // (~1.84e19) and far above any realistic Solana atomic amount.
    double atomic_d = amount_ui * swap_pow10(from->decimals) + 0.5;
    if (atomic_d >= 1e18) return false;
    uint64_t atomic = (uint64_t)atomic_d;
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

// POST /swap returns a base64 unsigned v0 versioned tx and a few metadata
// fields (`prioritizationFeeLamports`). Caller owns `tx_b64_out` and frees
// via free().
typedef struct {
    char    *tx_b64;             // heap, free()
    uint64_t priority_lamports;  // server may inflate from our requested 1
} jup_swap_t;

static bool jup_build_swap(const jup_quote_t *q, const char *user_pubkey,
                           jup_swap_t *out) {
    memset(out, 0, sizeof(*out));
    // Bound the user_pubkey before splicing into the JSON body. A real
    // base58 Solana pubkey is 32-44 ASCII chars and never contains "
    // or \, but the function takes a const char * with no documented
    // contract — refuse anything outside that range.
    if (!q || !user_pubkey) return false;
    size_t pk_len = strlen(user_pubkey);
    if (pk_len < 32 || pk_len > 44) return false;

    // body = { quoteResponse: <verbatim quote JSON>, userPublicKey, ... }
    // We hand-build the body so we don't pay an extra parse+serialize round
    // on the (verbatim) quote JSON. cJSON would otherwise reformat numbers.
    size_t body_cap = strlen(q->quote_json) + 256;
    char *body = malloc(body_cap);
    if (!body) return false;
    int n = snprintf(body, body_cap,
        "{\"quoteResponse\":%s,"
        "\"userPublicKey\":\"%s\","
        "\"wrapAndUnwrapSol\":true,"
        "\"dynamicComputeUnitLimit\":true,"
        "\"prioritizationFeeLamports\":1}",
        q->quote_json, user_pubkey);
    if (n < 0 || (size_t)n >= body_cap) { free(body); return false; }

    char *rsp = heap_caps_malloc(JUP_SWAP_RSP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rsp) { free(body); return false; }
    bool ok = https_post_json("https://quote-api.jup.ag/v6/swap",
                              body, rsp, JUP_SWAP_RSP_CAP);
    free(body);
    if (!ok) { free(rsp); return false; }

    cJSON *root = cJSON_Parse(rsp);
    if (!root) { free(rsp); ESP_LOGW(TAG, "swap: parse fail"); return false; }
    const cJSON *tx = cJSON_GetObjectItem(root, "swapTransaction");
    if (!cJSON_IsString(tx)) {
        cJSON_Delete(root); free(rsp);
        ESP_LOGW(TAG, "swap: missing swapTransaction");
        return false;
    }
    out->tx_b64 = strdup(tx->valuestring);
    const cJSON *pri = cJSON_GetObjectItem(root, "prioritizationFeeLamports");
    // Mirror the floor we requested in the body. A response without the
    // field (or with a non-number) means the server didn't inflate, so 1
    // lamport is what will actually settle.
    out->priority_lamports = cJSON_IsNumber(pri) ? (uint64_t)pri->valuedouble : 1;
    cJSON_Delete(root);
    free(rsp);
    return out->tx_b64 != NULL;
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

static volatile bool s_in_progress = false;

bool swap_request(const char *from_sym,
                  const char *to_sym,
                  double      amount_ui,
                  uint16_t    slippage_bps,
                  swap_result_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (from_sym) strlcpy(out->from_sym, from_sym, sizeof(out->from_sym));
    if (to_sym)   strlcpy(out->to_sym,   to_sym,   sizeof(out->to_sym));
    out->amount_in = amount_ui;

    if (s_in_progress) {
        out->status = SWAP_ERR_IN_PROGRESS;
        strlcpy(out->error_msg, "swap_in_progress", sizeof(out->error_msg));
        return false;
    }
    s_in_progress = true;

    swap_token_t a, b;
    if (!resolve_token(from_sym, &a) || !resolve_token(to_sym, &b)) {
        out->status = SWAP_ERR_QUOTE;
        strlcpy(out->error_msg, "unknown_symbol", sizeof(out->error_msg));
        s_in_progress = false; return false;
    }
    if (!has_balance(&a, amount_ui)) {
        out->status = SWAP_ERR_INSUFFICIENT;
        strlcpy(out->error_msg, "insufficient_balance", sizeof(out->error_msg));
        s_in_progress = false; return false;
    }

    uint16_t slip = clamp_slippage(slippage_bps, default_slippage(&a, &b));

    jup_quote_t q;
    if (!jup_get_quote(&a, &b, amount_ui, slip, &q)) {
        out->status = SWAP_ERR_QUOTE;
        strlcpy(out->error_msg, "quote_failed", sizeof(out->error_msg));
        s_in_progress = false; return false;
    }

    double scale_out  = swap_pow10(b.decimals);
    double amount_out = (double)q.out_atomic    / scale_out;
    double min_out    = (double)q.min_out_atomic / scale_out;

    swap_screen_args_t ui = {0};
    strlcpy(ui.from_sym, a.sym, sizeof(ui.from_sym));
    strlcpy(ui.to_sym,   b.sym, sizeof(ui.to_sym));
    ui.amount_in    = amount_ui;
    ui.amount_out   = amount_out;
    ui.min_out      = min_out;
    ui.slippage_bps = slip;
    // Conservative fee estimate for the approval modal:
    //   0.000005     = 5000-lamport priority floor (matches /swap request)
    //   0.000000001  = 1-lamport base signature fee (single signer)
    //   0.00203928   = ATA rent-exempt minimum, only if dest mint isn't held yet
    bool dest_has_ata = false;
    if (strcmp(b.sym, "SOL") == 0) {
        dest_has_ata = true;
    } else {
        const token_holding_t *toks = NULL; size_t n = 0;
        wallet_tokens(&toks, &n);
        for (size_t i = 0; i < n; ++i) {
            if (strcmp(toks[i].mint, b.mint) == 0) { dest_has_ata = true; break; }
        }
    }
    ui.fee_sol = 0.000005 + 0.000000001 + (dest_has_ata ? 0.0 : 0.00203928);

    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) {
        out->status = SWAP_ERR_QUOTE;
        strlcpy(out->error_msg, "no_sem", sizeof(out->error_msg));
        free(q.quote_json); s_in_progress = false; return false;
    }
    swap_ui_result_t ui_result = SWAP_UI_CANCEL_TIMEOUT;
    swap_screen_open(&ui, sem, &ui_result);
    xSemaphoreTake(sem, portMAX_DELAY);
    vSemaphoreDelete(sem);

    if (ui_result != SWAP_UI_CONFIRM) {
        out->status = SWAP_ERR_CANCELLED;
        strlcpy(out->error_msg, "cancelled", sizeof(out->error_msg));
        free(q.quote_json); s_in_progress = false; return false;
    }

    jup_swap_t s;
    if (!jup_build_swap(&q, wallet_pubkey(), &s)) {
        out->status = SWAP_ERR_BUILD;
        strlcpy(out->error_msg, "build_failed", sizeof(out->error_msg));
        free(q.quote_json); s_in_progress = false; return false;
    }
    free(q.quote_json);

    char *signed_b64 = malloc(SWAP_TX_B64_CAP);
    if (!signed_b64) {
        out->status = SWAP_ERR_BUILD;
        strlcpy(out->error_msg, "oom", sizeof(out->error_msg));
        free(s.tx_b64); s_in_progress = false; return false;
    }
    if (sign_v0_tx_in_place(s.tx_b64, signed_b64, SWAP_TX_B64_CAP) != 0) {
        out->status = SWAP_ERR_BUILD;
        strlcpy(out->error_msg, "sign_failed", sizeof(out->error_msg));
        free(signed_b64); free(s.tx_b64); s_in_progress = false; return false;
    }
    free(s.tx_b64);

    if (!rpc_send_tx(signed_b64, out->txid, sizeof(out->txid))) {
        out->status = SWAP_ERR_SUBMIT;
        strlcpy(out->error_msg, "submit_failed", sizeof(out->error_msg));
        free(signed_b64); s_in_progress = false; return false;
    }
    free(signed_b64);

    if (!rpc_wait_for_confirm(out->txid)) {
        out->status = SWAP_ERR_UNCONFIRMED;
        strlcpy(out->error_msg, "unconfirmed", sizeof(out->error_msg));
        s_in_progress = false; return false;
    }

    out->status     = SWAP_OK;
    out->amount_out = amount_out;
    wallet_request_refresh();
    s_in_progress = false;
    return true;
}

const char *swap_show_demo_for_test(void) {
    static const char *S[] = {
        "confirm", "cancel_release", "cancel_swipe", "cancel_timeout"
    };
    swap_screen_args_t args = {
        .from_sym = "USDC", .to_sym = "SOL",
        .amount_in = 5.0, .amount_out = 0.0431, .min_out = 0.0429,
        .slippage_bps = 50, .fee_sol = 0.00001,
    };
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) return "no_sem";
    swap_ui_result_t r = SWAP_UI_CANCEL_TIMEOUT;
    swap_screen_open(&args, sem, &r);
    xSemaphoreTake(sem, pdMS_TO_TICKS(35000));
    vSemaphoreDelete(sem);
    if (r >= 0 && r < (int)(sizeof(S)/sizeof(S[0]))) return S[r];
    return "unknown";
}

// ---------------------------------------------------------------------------
// v0 transaction signing
// ---------------------------------------------------------------------------

// Decode a base64 v0 versioned tx, locate the message bytes and the
// signature slot owned by our wallet, sign the message via wallet_sign(),
// and write the 64-byte signature into that slot. Re-encodes back to
// base64 in `out_b64` (NUL-terminated). Returns 0 on success, negative
// on any failure.
//
// Wire layout parsed:
//   [ compact-u16: numRequiredSignatures ]
//   [ numRequiredSignatures × 64-byte signature slots ]
//   [ message:
//       [ 1B prefix 0x80 ] [ 1B numRequiredSignatures ]
//       [ 1B numReadonlySigned ] [ 1B numReadonlyUnsigned ]
//       [ compact-u16: numStaticAccountKeys ]
//       [ numStaticAccountKeys × 32-byte pubkeys ]  ← signers are first n slots
//       ... rest of message
//   ]
static int sign_v0_tx_in_place(const char *in_b64, char *out_b64, size_t out_cap) {
    size_t in_b64_len = strlen(in_b64);
    uint8_t *raw = malloc(SWAP_TX_RAW_CAP);
    if (!raw) return -1;
    size_t raw_len = 0;
    int rc = mbedtls_base64_decode(raw, SWAP_TX_RAW_CAP, &raw_len,
                                    (const uint8_t *)in_b64, in_b64_len);
    if (rc != 0) { free(raw); ESP_LOGW(TAG, "tx b64 decode rc=%d", rc); return -1; }

    size_t cursor = 0;
    uint16_t num_sigs = 0;
    if (!compact_u16_read(raw, raw_len, &cursor, &num_sigs)) { free(raw); return -1; }
    if (num_sigs == 0 || num_sigs > 8)                       { free(raw); return -1; }
    size_t sigs_off = cursor;
    size_t msg_off  = sigs_off + (size_t)num_sigs * 64;
    if (msg_off >= raw_len)                                   { free(raw); return -1; }

    // Read message header: 1B prefix, 1B numReqSig, 1B numRoSig, 1B numRoUnsig
    if (msg_off + 4 >= raw_len) { free(raw); return -1; }
    cursor = msg_off + 4;
    uint16_t num_keys = 0;
    if (!compact_u16_read(raw, raw_len, &cursor, &num_keys)) { free(raw); return -1; }
    size_t keys_off = cursor;
    if (keys_off + (size_t)num_keys * 32 > raw_len)           { free(raw); return -1; }
    // Signers occupy the first `num_sigs` static keys. A malformed tx
    // claiming more signers than total keys would let the loop below
    // read past the keys array.
    if (num_sigs > num_keys)                                  { free(raw); return -1; }

    const uint8_t *mypub = wallet_pubkey_bytes();
    if (!mypub) { free(raw); return -1; }
    int slot = -1;
    for (uint16_t i = 0; i < num_sigs; ++i) {
        if (memcmp(raw + keys_off + (size_t)i * 32, mypub, 32) == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "our pubkey not found in first %u signers", num_sigs);
        free(raw);
        return -1;
    }

    uint8_t sig[64];
    if (!wallet_sign(raw + msg_off, raw_len - msg_off, sig)) {
        free(raw); return -1;
    }
    memcpy(raw + sigs_off + (size_t)slot * 64, sig, 64);

    size_t enc_len = 0;
    rc = mbedtls_base64_encode((uint8_t *)out_b64, out_cap, &enc_len,
                               raw, raw_len);
    free(raw);
    if (rc != 0) { ESP_LOGW(TAG, "tx b64 encode rc=%d", rc); return -1; }
    if (enc_len >= out_cap) return -1;
    out_b64[enc_len] = '\0';
    return 0;
}

// Diagnostic: sign the tx, then decode the result and verify the signature
// at our slot validates against the message bytes. Returns 0 on success,
// negative on failure (distinct codes for debugging).
int swap_test_sig_for_test(const char *in_b64, char *out_b64, size_t cap) {
    if (sign_v0_tx_in_place(in_b64, out_b64, cap) != 0) return -1;

    // Decode again and verify the signature at our slot.
    size_t b64_len = strlen(out_b64);
    uint8_t *raw = malloc(SWAP_TX_RAW_CAP);
    if (!raw) return -2;
    size_t raw_len = 0;
    if (mbedtls_base64_decode(raw, SWAP_TX_RAW_CAP, &raw_len,
                              (const uint8_t *)out_b64, b64_len) != 0) {
        free(raw); return -3;
    }
    size_t cursor = 0;
    uint16_t num_sigs = 0;
    compact_u16_read(raw, raw_len, &cursor, &num_sigs);
    size_t sigs_off = cursor;
    size_t msg_off  = sigs_off + (size_t)num_sigs * 64;
    cursor = msg_off + 4;
    uint16_t num_keys = 0;
    compact_u16_read(raw, raw_len, &cursor, &num_keys);
    size_t keys_off = cursor;
    // Mirror the same num_sigs <= num_keys bound the sign path enforces;
    // we just produced this buffer ourselves so the check is defensive
    // symmetry, not load-bearing.
    if (num_sigs > num_keys) { free(raw); return -4; }

    const uint8_t *mypub = wallet_pubkey_bytes();
    if (!mypub) { free(raw); return -4; }
    int slot = -1;
    for (uint16_t i = 0; i < num_sigs; ++i) {
        if (memcmp(raw + keys_off + (size_t)i * 32, mypub, 32) == 0) {
            slot = (int)i; break;
        }
    }
    if (slot < 0) { free(raw); return -4; }

    int ok = ed25519_verify(raw + sigs_off + (size_t)slot * 64,
                            raw + msg_off, raw_len - msg_off,
                            mypub);
    free(raw);
    return ok ? 0 : -5;
}

// sendTransaction with encoding=base64. Returns true and writes txid (base58)
// on success.
static bool rpc_send_tx(const char *signed_tx_b64, char *txid_out, size_t cap) {
    size_t req_cap = strlen(signed_tx_b64) + 256;
    char *req = malloc(req_cap);
    if (!req) return false;
    snprintf(req, req_cap,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sendTransaction\","
        "\"params\":[\"%s\","
        "{\"encoding\":\"base64\",\"skipPreflight\":false,"
        "\"preflightCommitment\":\"processed\"}]}",
        signed_tx_b64);

    // 4 KB rsp: a successful reply is ~130 bytes, but a preflight failure
    // returns the full simulator log under error.data.logs[] which routinely
    // runs 1-3 KB. https_post_json rejects truncation as failure, so a
    // small buffer would swallow the most useful debugging signal.
    char rsp[4096];
    char url[160];
    wallet_rpc_url(url, sizeof(url));
    bool ok = https_post_json(url, req, rsp, sizeof(rsp));
    free(req);
    if (!ok) return false;

    cJSON *root = cJSON_Parse(rsp);
    if (!root) { ESP_LOGW(TAG, "send: parse fail"); return false; }
    bool got = false;
    const cJSON *result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsString(result) && result->valuestring && result->valuestring[0]) {
        strlcpy(txid_out, result->valuestring, cap);
        got = true;
    } else {
        const cJSON *err = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsObject(err)) {
            const cJSON *msg = cJSON_GetObjectItem(err, "message");
            if (cJSON_IsString(msg)) ESP_LOGW(TAG, "send err: %s", msg->valuestring);
        }
    }
    cJSON_Delete(root);
    return got;
}

// Polls getSignatureStatuses every 800 ms up to 30 s. Returns true once
// the tx has a non-null status; false on timeout.
static bool rpc_wait_for_confirm(const char *txid) {
    ESP_LOGI(TAG, "wait confirm %.16s...", txid ? txid : "(null)");
    char url[160];
    wallet_rpc_url(url, sizeof(url));
    for (int i = 0; i < 38; ++i) {
        char req[256];
        snprintf(req, sizeof(req),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getSignatureStatuses\","
            "\"params\":[[\"%s\"],{\"searchTransactionHistory\":false}]}",
            txid);
        char rsp[1024];
        if (https_post_json(url, req, rsp, sizeof(rsp))) {
            cJSON *root = cJSON_Parse(rsp);
            if (root) {
                const cJSON *result = cJSON_GetObjectItem(root, "result");
                const cJSON *value  = result ? cJSON_GetObjectItem(result, "value") : NULL;
                // Cluster has seen the tx as soon as value[0] is an object;
                // err vs success is the caller's concern.
                bool landed = false;
                if (cJSON_IsArray(value) && cJSON_GetArraySize(value) > 0) {
                    const cJSON *st = cJSON_GetArrayItem(value, 0);
                    if (cJSON_IsObject(st)) landed = true;
                }
                cJSON_Delete(root);
                if (landed) return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    ESP_LOGW(TAG, "wait confirm timeout %.16s...", txid ? txid : "(null)");
    return false;
}

bool swap_dry_run_for_test(const char *from_sym, const char *to_sym,
                           double amount_ui, uint16_t slippage_bps,
                           char *out_json, size_t cap) {
    swap_token_t a, b;
    if (!resolve_token(from_sym, &a) || !resolve_token(to_sym, &b)) {
        snprintf(out_json, cap, "{\"error\":\"unknown_symbol\"}"); return false;
    }
    if (!has_balance(&a, amount_ui)) {
        snprintf(out_json, cap, "{\"error\":\"insufficient_balance\"}"); return false;
    }
    uint16_t slip = clamp_slippage(slippage_bps, default_slippage(&a, &b));
    jup_quote_t q;
    if (!jup_get_quote(&a, &b, amount_ui, slip, &q)) {
        snprintf(out_json, cap, "{\"error\":\"quote_failed\"}"); return false;
    }
    double scale = swap_pow10(b.decimals);
    snprintf(out_json, cap,
        "{\"from\":\"%s\",\"to\":\"%s\",\"in\":%.6f,\"out\":%.6f,"
        "\"min\":%.6f,\"slip_bps\":%u}",
        a.sym, b.sym, amount_ui,
        (double)q.out_atomic     / scale,
        (double)q.min_out_atomic / scale,
        (unsigned)slip);
    free(q.quote_json);
    return true;
}
