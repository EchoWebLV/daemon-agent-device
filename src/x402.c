// ---------------------------------------------------------------------------
//  x402 HTTP client. See x402.h.
//
//  The interesting part is the 402 path: the facilitator (e.g. sol.blockrun)
//  answers with a payment-required description, either in a header or the
//  body. We decode it, build + sign a Solana USDC TransferChecked, wrap that
//  in the x402 payment envelope JSON, base64 it, and retry with a
//  PAYMENT-SIGNATURE header. Same wire protocol as the Chrome extension.
//
//  Everything is sized up-front and NUL-safe; we never grow buffers mid-call.
//  The three possible payment-required header spellings and the scratch
//  buffers for the envelope + base64 go on the heap for the duration of the
//  call (too big for the default task stack); everything else is stack.
// ---------------------------------------------------------------------------
#include "x402.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#include "base58.h"
#include "secrets.h"
#include "solana_tx.h"
#include "wallet.h"
#include "wifi_sta.h"

static const char *TAG = "x402";

// ---------------------------------------------------------------------------
// Chrome-ext constants. Solana mainnet cluster id + USDC mint.
// ---------------------------------------------------------------------------
static const char *SOLANA_NETWORK_ID = "solana:5eykt4UsFv8P8NJdTREpY1vzqKqZKvdp";
static const char *USDC_MINT         = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
#define USDC_DECIMALS 6

// Max length of a single captured response header. Payment-required headers
// carry base64(JSON); with the x402 extensions.bazaar schema that can reach
// 4–5 KB, so 6 KB gives us headroom.
#define X402_HDR_CAP 6144

// Scratch sizes for the payment-envelope pipeline (decoded description,
// envelope JSON we build, and its base64 wrapper for the header value).
// Base64 inflates by ~34%; sizes are upper bounds that comfortably fit the
// observed shapes.
#define X402_DESC_CAP 6144    // decoded payment-required JSON
#define X402_ENV_CAP  6144    // envelope JSON
#define X402_B64_CAP  12288   // base64 of envelope

// ---------------------------------------------------------------------------
// Small body-only event handler, used for the two helper RPC calls
// (getLatestBlockhash + getTokenAccountsByOwner-by-mint).
// ---------------------------------------------------------------------------
typedef struct { char *buf; size_t cap; size_t len; } rpc_body_t;

static esp_err_t on_rpc_event(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    rpc_body_t *r = (rpc_body_t *)evt->user_data;
    if (!r || !r->buf || r->cap == 0) return ESP_OK;
    size_t room = r->cap - 1 - r->len;
    if (room == 0) return ESP_OK;
    size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    memcpy(r->buf + r->len, evt->data, take);
    r->len += take;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

static bool rpc_call(const char *payload, char *out, size_t out_cap) {
    if (!wifi_sta_is_connected()) return false;
    if (out_cap == 0) return false;
    out[0] = '\0';

    char url[160];
    wallet_rpc_url(url, sizeof(url));

    rpc_body_t rb = { .buf = out, .cap = out_cap, .len = 0 };
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = on_rpc_event,
        .user_data         = &rb,
        .timeout_ms        = 15000,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return false;
    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_post_field(http, payload, (int)strlen(payload));

    esp_err_t err  = esp_http_client_perform(http);
    int       code = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);

    if (err != ESP_OK) { ESP_LOGW(TAG, "rpc perform: %s", esp_err_to_name(err)); return false; }
    if (code != 200)   { ESP_LOGW(TAG, "rpc HTTP %d", code);                      return false; }
    return true;
}

// ---------------------------------------------------------------------------
// RPC helpers used while building the payment.
// ---------------------------------------------------------------------------
static bool fetch_recent_blockhash(uint8_t out[32]) {
    static const char *REQ =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getLatestBlockhash\",\"params\":[]}";
    char body[512];
    if (!rpc_call(REQ, body, sizeof(body))) return false;

    cJSON *root = cJSON_Parse(body);
    if (!root) { ESP_LOGW(TAG, "blockhash: json parse"); return false; }

    bool ok = false;
    cJSON *res = cJSON_GetObjectItem(root, "result");
    cJSON *val = res ? cJSON_GetObjectItem(res, "value") : NULL;
    cJSON *bh  = val ? cJSON_GetObjectItem(val, "blockhash") : NULL;
    if (cJSON_IsString(bh) && bh->valuestring) {
        ok = (base58_decode(bh->valuestring, out, 32) == 32);
    }
    cJSON_Delete(root);
    if (!ok) ESP_LOGW(TAG, "blockhash: missing/invalid");
    return ok;
}

static bool fetch_usdc_ata(const char *owner_b58, char *out, size_t cap) {
    if (cap) out[0] = '\0';
    char payload[256];
    int n = snprintf(payload, sizeof(payload),
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"method\":\"getTokenAccountsByOwner\",\"params\":[\"%s\","
        "{\"mint\":\"%s\"},{\"encoding\":\"jsonParsed\"}]}",
        owner_b58, USDC_MINT);
    if (n < 0 || n >= (int)sizeof(payload)) return false;

    // One token account on-chain is ~1 KB of jsonParsed metadata; 4 KB is
    // a safe upper bound for the single-account response we parse. PSRAM
    // keeps internal heap free for the concurrent TLS session.
    char *body = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!body) return false;
    bool ok = false;
    if (rpc_call(payload, body, 4096)) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            cJSON *res = cJSON_GetObjectItem(root, "result");
            cJSON *val = res ? cJSON_GetObjectItem(res, "value") : NULL;
            if (cJSON_IsArray(val) && cJSON_GetArraySize(val) > 0) {
                cJSON *item = cJSON_GetArrayItem(val, 0);
                cJSON *pk   = item ? cJSON_GetObjectItem(item, "pubkey") : NULL;
                if (cJSON_IsString(pk) && pk->valuestring) {
                    strlcpy(out, pk->valuestring, cap);
                    ok = true;
                }
            }
            cJSON_Delete(root);
        }
    }
    free(body);
    return ok;
}

// ---------------------------------------------------------------------------
// mbedtls wrappers — base64 that returns a NUL-terminated C string.
// ---------------------------------------------------------------------------
static bool b64_encode(const uint8_t *in, size_t in_len,
                      char *out, size_t out_cap, size_t *out_len) {
    size_t written = 0;
    int rc = mbedtls_base64_encode((uint8_t *)out, out_cap, &written, in, in_len);
    if (rc != 0) {
        ESP_LOGW(TAG, "b64 encode rc=%d (need %u have %u)",
                 rc, (unsigned)written, (unsigned)out_cap);
        return false;
    }
    if (written >= out_cap) return false;
    out[written] = '\0';
    if (out_len) *out_len = written;
    return true;
}

static bool b64_decode(const char *in, size_t in_len,
                      char *out, size_t out_cap, size_t *out_len) {
    size_t written = 0;
    int rc = mbedtls_base64_decode((uint8_t *)out, out_cap, &written,
                                   (const uint8_t *)in, in_len);
    if (rc != 0 || written == 0) return false;
    if (written >= out_cap) return false;
    out[written] = '\0';
    if (out_len) *out_len = written;
    return true;
}

// ---------------------------------------------------------------------------
// Header state captured during the first (possibly 402) response.
// ---------------------------------------------------------------------------
typedef struct {
    char   *body_buf;       // caller-owned
    size_t  body_cap;
    size_t  body_len;

    // Each may hold a ~4 KB base64 blob when present; stay unallocated
    // (NULL) when we don't need them (e.g. the retry path).
    char   *pr;             // "payment-required" header value
    char   *xpr;            // "x-payment-required" header value
    char   *wa;             // "www-authenticate" header value
} x402_state_t;

static esp_err_t on_x402_event(esp_http_client_event_t *evt) {
    x402_state_t *s = (x402_state_t *)evt->user_data;
    if (!s) return ESP_OK;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER: {
            if (!evt->header_key || !evt->header_value) break;
            if (s->pr && strcasecmp(evt->header_key, "payment-required") == 0) {
                strlcpy(s->pr, evt->header_value, X402_HDR_CAP);
            } else if (s->xpr && strcasecmp(evt->header_key, "x-payment-required") == 0) {
                strlcpy(s->xpr, evt->header_value, X402_HDR_CAP);
            } else if (s->wa && strcasecmp(evt->header_key, "www-authenticate") == 0) {
                strlcpy(s->wa, evt->header_value, X402_HDR_CAP);
            }
            break;
        }
        case HTTP_EVENT_ON_DATA: {
            if (!s->body_buf || s->body_cap == 0) break;
            size_t room = s->body_cap - 1 - s->body_len;
            if (room == 0) break;
            size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
            memcpy(s->body_buf + s->body_len, evt->data, take);
            s->body_len += take;
            s->body_buf[s->body_len] = '\0';
            break;
        }
        default: break;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Try every known spot for the payment-required description.  Writes the
// decoded JSON (NUL-terminated) into `out` and returns true on success.
// ---------------------------------------------------------------------------
static bool decode_payment_description(const x402_state_t *s,
                                       char *out, size_t out_cap) {
    if (!out || out_cap == 0) return false;
    out[0] = '\0';

    // 1. `payment-required` header (sol.blockrun.ai uses this one).
    if (s->pr && s->pr[0] &&
        b64_decode(s->pr, strlen(s->pr), out, out_cap, NULL)) {
        return true;
    }
    // 2. `x-payment-required` header.
    if (s->xpr && s->xpr[0] &&
        b64_decode(s->xpr, strlen(s->xpr), out, out_cap, NULL)) {
        return true;
    }
    // 3. `WWW-Authenticate: X402 requirements="<b64>"`.
    if (s->wa && s->wa[0]) {
        const char *q1 = strchr(s->wa, '"');
        const char *q2 = q1 ? strrchr(s->wa, '"') : NULL;
        if (q1 && q2 && q2 > q1 + 1) {
            size_t blen = (size_t)(q2 - (q1 + 1));
            if (b64_decode(q1 + 1, blen, out, out_cap, NULL)) return true;
        }
    }
    // 4. Body itself is the JSON (no header spelling matched).
    if (s->body_len > 0 && s->body_len < out_cap) {
        memcpy(out, s->body_buf, s->body_len);
        out[s->body_len] = '\0';
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Select the Solana-compatible "accepts" entry. Falls back to accepts[0]
// if none matches our network id.
// ---------------------------------------------------------------------------
static cJSON *pick_solana_accept(cJSON *payment_required) {
    cJSON *arr = cJSON_GetObjectItem(payment_required, "accepts");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) return NULL;
    cJSON *fallback = cJSON_GetArrayItem(arr, 0);
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *net = cJSON_GetObjectItem(it, "network");
        if (cJSON_IsString(net) && net->valuestring &&
            strcmp(net->valuestring, SOLANA_NETWORK_ID) == 0) {
            return it;
        }
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Build the base64-encoded x402 payment envelope to be sent back as
// PAYMENT-SIGNATURE. Returns false on any failure; on success `out_b64` is
// a NUL-terminated string and `out_amount_atomic` holds the charged amount.
// ---------------------------------------------------------------------------
static bool build_payment_header(const char *desc_json,
                                 const char *resource_url,
                                 char       *out_b64,
                                 size_t      out_b64_cap,
                                 uint64_t   *out_amount_atomic) {
    bool ok = false;
    cJSON *pr  = NULL;
    char  *env = NULL;
    char  *ext_str = NULL, *exts_str = NULL;

    pr = cJSON_Parse(desc_json);
    if (!pr) { ESP_LOGW(TAG, "402 desc: parse failed"); goto out; }

    cJSON *opt = pick_solana_accept(pr);
    if (!opt) { ESP_LOGW(TAG, "402 desc: no accepts entry"); goto out; }

    const cJSON *pay_to_j    = cJSON_GetObjectItem(opt, "payTo");
    const cJSON *amount_j    = cJSON_GetObjectItem(opt, "amount");
    const cJSON *amount_max_j= cJSON_GetObjectItem(opt, "maxAmountRequired");
    cJSON       *extra_j     = cJSON_GetObjectItem(opt, "extra");
    const cJSON *fee_payer_j = extra_j ? cJSON_GetObjectItem(extra_j, "feePayer") : NULL;
    const cJSON *timeout_j   = cJSON_GetObjectItem(opt, "maxTimeoutSeconds");
    cJSON       *exts_node   = cJSON_GetObjectItem(pr, "extensions");

    const char *pay_to    = (cJSON_IsString(pay_to_j)    && pay_to_j->valuestring)    ? pay_to_j->valuestring    : NULL;
    const char *fee_payer = (cJSON_IsString(fee_payer_j) && fee_payer_j->valuestring) ? fee_payer_j->valuestring : NULL;
    const char *amount_s  = NULL;
    if (cJSON_IsString(amount_j)     && amount_j->valuestring)     amount_s = amount_j->valuestring;
    else if (cJSON_IsString(amount_max_j) && amount_max_j->valuestring) amount_s = amount_max_j->valuestring;

    if (!pay_to || !fee_payer || !amount_s) {
        ESP_LOGW(TAG, "402 desc: missing payTo/amount/feePayer");
        goto out;
    }
    uint64_t amount_atomic = strtoull(amount_s, NULL, 10);
    int      timeout_sec   = cJSON_IsNumber(timeout_j) ? (int)timeout_j->valuedouble : 300;

    // Source ATA — cached by wallet_refresh(). If we haven't seen one the
    // wallet is either empty or still warming up; either way this call fails.
    const char *source_ata_b58 = wallet_usdc_ata();
    if (!source_ata_b58 || !source_ata_b58[0]) {
        ESP_LOGW(TAG, "wallet has no USDC ATA cached");
        goto out;
    }

    char dest_ata_b58[64] = {0};
    if (!fetch_usdc_ata(pay_to, dest_ata_b58, sizeof(dest_ata_b58))) {
        ESP_LOGW(TAG, "destination %s has no USDC ATA", pay_to);
        goto out;
    }

    uint8_t fee_payer_key[32], source_ata[32], dest_ata[32], mint_key[32], blockhash[32];
    if (base58_decode(fee_payer,       fee_payer_key, 32) != 32) goto out;
    if (base58_decode(source_ata_b58,  source_ata,    32) != 32) goto out;
    if (base58_decode(dest_ata_b58,    dest_ata,      32) != 32) goto out;
    if (base58_decode(USDC_MINT,       mint_key,      32) != 32) goto out;
    if (!fetch_recent_blockhash(blockhash))                      goto out;

    solana_tx_input_t in = {
        .fee_payer      = fee_payer_key,
        .wallet_owner   = wallet_pubkey_bytes(),
        .source_ata     = source_ata,
        .dest_ata       = dest_ata,
        .mint           = mint_key,
        .blockhash      = blockhash,
        .amount_atomic  = amount_atomic,
        .mint_decimals  = USDC_DECIMALS,
        .cu_limit       = 8000,
        .cu_price_micro = 1,
    };
    if (!in.wallet_owner) { ESP_LOGW(TAG, "no wallet pubkey"); goto out; }

    char tx_b64[768];
    int tx_len = solana_build_signed_tx_base64(&in, tx_b64, sizeof(tx_b64));
    if (tx_len <= 0) { ESP_LOGW(TAG, "solana tx build failed"); goto out; }

    // Re-serialize `extra` and `extensions` so the envelope echoes them back
    // verbatim (facilitator validates the signature over these fields).
    ext_str  = extra_j  ? cJSON_PrintUnformatted(extra_j)  : strdup("{}");
    exts_str = exts_node ? cJSON_PrintUnformatted(exts_node) : strdup("{}");
    if (!ext_str || !exts_str) goto out;

    env = (char *)heap_caps_malloc(X402_ENV_CAP, MALLOC_CAP_SPIRAM);
    if (!env) goto out;
    int n = snprintf(env, X402_ENV_CAP,
        "{\"x402Version\":2,"
        "\"resource\":{\"url\":\"%s\","
          "\"description\":\"Daemon x402\","
          "\"mimeType\":\"application/json\"},"
        "\"accepted\":{"
          "\"scheme\":\"exact\","
          "\"network\":\"%s\","
          "\"amount\":\"%s\","
          "\"asset\":\"%s\","
          "\"payTo\":\"%s\","
          "\"maxTimeoutSeconds\":%d,"
          "\"extra\":%s"
        "},"
        "\"payload\":{\"transaction\":\"%s\"},"
        "\"extensions\":%s"
        "}",
        resource_url ? resource_url : "",
        SOLANA_NETWORK_ID,
        amount_s,
        USDC_MINT,
        pay_to,
        timeout_sec,
        ext_str,
        tx_b64,
        exts_str);
    if (n < 0 || n >= (int)X402_ENV_CAP) {
        ESP_LOGW(TAG, "envelope overflow (%d)", n);
        goto out;
    }

    if (!b64_encode((const uint8_t *)env, (size_t)n, out_b64, out_b64_cap, NULL)) {
        ESP_LOGW(TAG, "envelope base64 failed");
        goto out;
    }
    if (out_amount_atomic) *out_amount_atomic = amount_atomic;
    ok = true;

out:
    if (pr)       cJSON_Delete(pr);
    if (env)      free(env);
    if (ext_str)  free(ext_str);
    if (exts_str) free(exts_str);
    return ok;
}

// ---------------------------------------------------------------------------
// Public entry point.
// ---------------------------------------------------------------------------
void x402_post(const char *url,
               const char *json_body,
               const char *auth_bearer,
               char       *body_buf,
               size_t      body_cap,
               x402_result_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (body_buf && body_cap) body_buf[0] = '\0';

    if (!url || !json_body || !body_buf || body_cap == 0) {
        strlcpy(out->error, "bad args", sizeof(out->error));
        return;
    }
    if (!wifi_sta_is_connected()) {
        strlcpy(out->error, "no wifi", sizeof(out->error));
        return;
    }

    // ---- Phase 1: initial POST (capturing potential 402 headers) --------
    // PSRAM: 3 × 6 KB header scratch is a big chunk of internal heap during
    // a live TLS session. The header scratch is only touched by the event
    // handler + the post-request decoder — no DMA, safe in PSRAM.
    char *pr  = (char *)heap_caps_calloc(1, X402_HDR_CAP, MALLOC_CAP_SPIRAM);
    char *xpr = (char *)heap_caps_calloc(1, X402_HDR_CAP, MALLOC_CAP_SPIRAM);
    char *wa  = (char *)heap_caps_calloc(1, X402_HDR_CAP, MALLOC_CAP_SPIRAM);
    if (!pr || !xpr || !wa) {
        free(pr); free(xpr); free(wa);
        strlcpy(out->error, "oom", sizeof(out->error));
        return;
    }

    x402_state_t state = {
        .body_buf = body_buf, .body_cap = body_cap, .body_len = 0,
        .pr = pr, .xpr = xpr, .wa = wa,
    };

    esp_http_client_config_t cfg1 = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = on_x402_event,
        .user_data         = &state,
        .timeout_ms        = 60000,
        // The 402 response carries a base64(JSON) payment-required header
        // that can reach 4–5 KB on sol.blockrun.ai; esp_http_client's
        // default 512-byte header buffer truncates it and the facilitator
        // ends up rejecting the retry with another 402. 8 KB leaves
        // headroom for content-type + auth + other standard response
        // headers alongside it.
        .buffer_size       = 8192,
    };
    esp_http_client_handle_t h1 = esp_http_client_init(&cfg1);
    if (!h1) {
        strlcpy(out->error, "client init", sizeof(out->error));
        goto done;
    }
    esp_http_client_set_header(h1, "Content-Type", "application/json");
    if (auth_bearer && auth_bearer[0]) {
        char h[160];
        snprintf(h, sizeof(h), "Bearer %s", auth_bearer);
        esp_http_client_set_header(h1, "Authorization", h);
    }
    esp_http_client_set_post_field(h1, json_body, (int)strlen(json_body));

    esp_err_t err1 = esp_http_client_perform(h1);
    int       code1 = esp_http_client_get_status_code(h1);
    esp_http_client_cleanup(h1);

    if (err1 != ESP_OK) {
        snprintf(out->error, sizeof(out->error), "perform: %s", esp_err_to_name(err1));
        goto done;
    }

    // ---- Phase 2: if 402, build payment and retry ------------------------
    if (code1 == 402) {
        char *desc = (char *)heap_caps_malloc(X402_DESC_CAP, MALLOC_CAP_SPIRAM);
        char *b64  = (char *)heap_caps_malloc(X402_B64_CAP,  MALLOC_CAP_SPIRAM);
        if (!desc || !b64) {
            free(desc); free(b64);
            strlcpy(out->error, "oom", sizeof(out->error));
            out->status = 402;
            goto done;
        }
        bool have_desc = decode_payment_description(&state, desc, X402_DESC_CAP);
        if (!have_desc) {
            ESP_LOGW(TAG, "402 without decodable description");
            strlcpy(out->error, "402 no description", sizeof(out->error));
            out->status = 402;
            free(desc); free(b64);
            goto done;
        }

        uint64_t paid_atomic = 0;
        bool built = build_payment_header(desc, url, b64, X402_B64_CAP, &paid_atomic);
        free(desc);
        if (!built) {
            strlcpy(out->error, "payment build failed", sizeof(out->error));
            out->status = 402;
            free(b64);
            goto done;
        }

        // Reset body buffer for the retry.
        body_buf[0]    = '\0';
        state.body_len = 0;
        // Don't need the header slots anymore; avoid re-populating them.
        state.pr = state.xpr = state.wa = NULL;

        esp_http_client_config_t cfg2 = {
            .url               = url,
            .method            = HTTP_METHOD_POST,
            .transport_type    = HTTP_TRANSPORT_OVER_SSL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler     = on_x402_event,
            .user_data         = &state,
            .timeout_ms        = 60000,
            // Phase-2 retry carries our base64(payment envelope) in the
            // PAYMENT-SIGNATURE request header — that envelope can hit
            // ~12 KB. The default 512-byte TX buffer truncates it on the
            // wire and the facilitator sees a garbled signature, then
            // replies 402 again. Size TX for the worst case, keep RX at
            // 8 KB so the 200-OK response headers still fit comfortably.
            .buffer_size       = 8192,
            .buffer_size_tx    = 16384,
        };
        esp_http_client_handle_t h2 = esp_http_client_init(&cfg2);
        if (!h2) {
            strlcpy(out->error, "retry init", sizeof(out->error));
            free(b64);
            goto done;
        }
        esp_http_client_set_header(h2, "Content-Type",      "application/json");
        esp_http_client_set_header(h2, "PAYMENT-SIGNATURE", b64);
        if (auth_bearer && auth_bearer[0]) {
            char h[160];
            snprintf(h, sizeof(h), "Bearer %s", auth_bearer);
            esp_http_client_set_header(h2, "Authorization", h);
        }
        esp_http_client_set_post_field(h2, json_body, (int)strlen(json_body));

        esp_err_t err2  = esp_http_client_perform(h2);
        int       code2 = esp_http_client_get_status_code(h2);
        esp_http_client_cleanup(h2);
        free(b64);

        out->status   = code2;
        out->body_len = state.body_len;
        if (err2 != ESP_OK) {
            snprintf(out->error, sizeof(out->error), "retry: %s", esp_err_to_name(err2));
        } else if (code2 == 200) {
            // Report the real USDC cost we committed to.
            out->cost_usd = (double)paid_atomic / 1e6;
            ESP_LOGI(TAG, "paid %.5f USDC", out->cost_usd);
        } else {
            snprintf(out->error, sizeof(out->error), "retry status %d", code2);
        }
        goto done;
    }

    // ---- No payment required (2xx or some other error) -------------------
    out->status   = code1;
    out->body_len = state.body_len;
    if (code1 != 200) {
        snprintf(out->error, sizeof(out->error), "status %d", code1);
    }

done:
    free(pr); free(xpr); free(wa);
}
