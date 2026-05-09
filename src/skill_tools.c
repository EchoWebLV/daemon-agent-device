// ---------------------------------------------------------------------------
//  Skill tools implementation. See skill_tools.h.
//
//  Five handlers:
//    http_request      — generic HTTPS, ${VAR} expansion against creds
//    x402_pay          — sign+settle for non-standard x402 v2 payments
//                        (SP3ND-style: external feePayer + memo)
//    secret_get/set    — per-skill credential read/write
//    solana_get_pubkey — device wallet pubkey
// ---------------------------------------------------------------------------
#include "skill_tools.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "mbedtls/base64.h"

#include "base58.h"
#include "skill_store.h"
#include "tls_lock.h"
#include "wallet.h"
#include "x402.h"

static const char *TAG = "skill_tools";

#define HTTP_RSP_BUF        (32 * 1024)   // up to 32 KB body returned to AI
#define HTTP_HEADER_TOTAL   2048
#define X402_FACILITATOR_DEFAULT "https://facilitator.payai.network"
#define USDC_MINT_B58       "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
#define MEMO_PROGRAM_B58    "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr"
#define TOKEN_PROGRAM_B58   "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
#define COMPUTE_BUDGET_B58  "ComputeBudget111111111111111111111111111111"

// ---------------------------------------------------------------------------
// Bounded write-cursor (mirrors the pattern in solana_tx.c).
// ---------------------------------------------------------------------------
typedef struct { uint8_t *buf; size_t cap; size_t len; bool overflow; } wc_t;

static void wc_bytes(wc_t *w, const uint8_t *src, size_t n) {
    if (w->len + n > w->cap) { w->overflow = true; return; }
    memcpy(w->buf + w->len, src, n);
    w->len += n;
}
static void wc_u8 (wc_t *w, uint8_t v)  { wc_bytes(w, &v, 1); }
static void wc_u32_le(wc_t *w, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = (uint8_t)(v >> (i * 8));
    wc_bytes(w, b, 4);
}
static void wc_u64_le(wc_t *w, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (i * 8));
    wc_bytes(w, b, 8);
}
static void wc_shortvec(wc_t *w, uint16_t v) {
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v != 0) byte |= 0x80;
        wc_u8(w, byte);
    } while (v != 0);
}

// ---------------------------------------------------------------------------
// JSON-escape helper (writes to a stretchy buffer). Pasted-down minimal
// version — only escapes the chars that break JSON strings.
// ---------------------------------------------------------------------------
static void append_json_escaped(char **buf, size_t *len, size_t *cap,
                                const char *s, size_t s_len) {
    for (size_t i = 0; i < s_len; ++i) {
        char esc[8];
        int  el;
        unsigned char c = (unsigned char)s[i];
        if      (c == '"' ) { esc[0] = '\\'; esc[1] = '"';  el = 2; }
        else if (c == '\\') { esc[0] = '\\'; esc[1] = '\\'; el = 2; }
        else if (c == '\n') { esc[0] = '\\'; esc[1] = 'n';  el = 2; }
        else if (c == '\r') { esc[0] = '\\'; esc[1] = 'r';  el = 2; }
        else if (c == '\t') { esc[0] = '\\'; esc[1] = 't';  el = 2; }
        else if (c < 0x20)  { el = snprintf(esc, sizeof(esc), "\\u%04x", c); }
        else                { esc[0] = (char)c; el = 1; }

        if (*len + (size_t)el + 1 >= *cap) {
            size_t new_cap = (*cap) * 2;
            if (new_cap < *len + (size_t)el + 16) new_cap = *len + (size_t)el + 16;
            char *r = realloc(*buf, new_cap);
            if (!r) return;
            *buf = r;
            *cap = new_cap;
        }
        memcpy(*buf + *len, esc, (size_t)el);
        *len += (size_t)el;
        (*buf)[*len] = '\0';
    }
}

// ---------------------------------------------------------------------------
// ${VAR} substitution — searches all enabled markdown-kind skills' NVS
// namespaces for VAR. First match wins. If a VAR is referenced but not
// set, returns NULL with `err` filled.
//
// Caller frees the returned buffer.
// ---------------------------------------------------------------------------
static char *substitute_vars(const char *in, char *err, size_t err_cap) {
    if (!in) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    size_t in_len = strlen(in);
    size_t cap    = in_len + 1024;
    char  *out    = malloc(cap);
    if (!out) { snprintf(err, err_cap, "oom"); return NULL; }
    size_t o = 0;

    char ids[16][SKILL_ID_MAX];
    int  n_ids = skill_store_list_enabled(ids, 16);

    for (size_t i = 0; i < in_len;) {
        // Detect "${"
        if (in[i] == '$' && i + 1 < in_len && in[i+1] == '{') {
            // Find closing "}"
            size_t j = i + 2;
            while (j < in_len && in[j] != '}') ++j;
            if (j >= in_len) {
                snprintf(err, err_cap, "unterminated_var");
                free(out);
                return NULL;
            }
            size_t name_len = j - (i + 2);
            if (name_len == 0 || name_len >= 64) {
                snprintf(err, err_cap, "bad_var_name");
                free(out);
                return NULL;
            }
            char name[65];
            memcpy(name, in + i + 2, name_len);
            name[name_len] = '\0';

            // Validate name charset
            bool nameok = (name[0] >= 'A' && name[0] <= 'Z');
            for (size_t k = 1; nameok && k < name_len; ++k) {
                char c = name[k];
                nameok = (c == '_' ||
                          (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9'));
            }
            if (!nameok) {
                snprintf(err, err_cap, "bad_var_name:%s", name);
                free(out);
                return NULL;
            }

            // Look up across enabled skills.
            char value[257];
            int  found = 0;
            for (int s = 0; s < n_ids; ++s) {
                if (skill_store_get_cred(ids[s], name, value, sizeof(value)) > 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                snprintf(err, err_cap, "unset:%s", name);
                free(out);
                return NULL;
            }

            size_t v_len = strlen(value);
            if (o + v_len + 1 > cap) {
                size_t new_cap = (cap + v_len + 1) * 2;
                char *r = realloc(out, new_cap);
                if (!r) { snprintf(err, err_cap, "oom"); free(out); return NULL; }
                out = r;
                cap = new_cap;
            }
            memcpy(out + o, value, v_len);
            o += v_len;
            i = j + 1;  // skip past '}'
            continue;
        }

        if (o + 1 + 1 > cap) {
            size_t new_cap = cap * 2;
            char *r = realloc(out, new_cap);
            if (!r) { snprintf(err, err_cap, "oom"); free(out); return NULL; }
            out = r;
            cap = new_cap;
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
    return out;
}

// Walks a cJSON tree and substitutes ${VAR} in any string leaf in-place.
// Returns true on success. On failure returns false with err filled.
static bool substitute_in_json(cJSON *node, char *err, size_t err_cap) {
    if (!node) return true;
    if (cJSON_IsString(node) && node->valuestring) {
        char *s = substitute_vars(node->valuestring, err, err_cap);
        if (!s) return false;
        cJSON_SetValuestring(node, s);
        free(s);
        return true;
    }
    if (cJSON_IsArray(node) || cJSON_IsObject(node)) {
        cJSON *c = NULL;
        cJSON_ArrayForEach(c, node) {
            if (!substitute_in_json(c, err, err_cap)) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// http_request handler
// ---------------------------------------------------------------------------
//
// Args:
//   {
//     "method":  "GET" | "POST" | "PUT" | "DELETE" | "PATCH",
//     "url":     "https://...",
//     "headers": { "X-API-Key": "${SP3ND_API_KEY}", ... }   // optional
//     "body":    "raw string"  OR  { ...JSON object... }    // optional
//   }
//
// Behavior:
//   - ${VAR} references in url, header values, and body strings are
//     expanded against any enabled markdown skill's stored credentials.
//   - 402 responses are returned RAW for the AI to feed into x402_pay.
//
// Response:
//   { "status": <int>, "body": "<response body>" }
//   { "error":  "<message>" }                               // on failure
// ---------------------------------------------------------------------------

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;
} body_capture_t;

static esp_err_t body_event_cb(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    body_capture_t *bc = (body_capture_t *)evt->user_data;
    if (!bc || bc->overflow) return ESP_OK;
    size_t take = (size_t)evt->data_len;
    if (bc->len + take >= bc->cap) {
        take = bc->cap - bc->len - 1;
        bc->overflow = true;
    }
    if (take > 0) {
        memcpy(bc->buf + bc->len, evt->data, take);
        bc->len += take;
        bc->buf[bc->len] = '\0';
    }
    return ESP_OK;
}

static int do_http_request(const char *method, const char *url,
                           cJSON *headers, const char *body, size_t body_len,
                           char *rsp, size_t rsp_cap, int *status_out) {
    *status_out = 0;
    rsp[0] = '\0';

    body_capture_t bc = { .buf = rsp, .cap = rsp_cap, .len = 0 };

    esp_http_client_config_t cfg = {
        .url            = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms     = 30000,
        .event_handler  = body_event_cb,
        .user_data      = &bc,
        .buffer_size    = 4096,
        .buffer_size_tx = 4096,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return -1;

    esp_http_client_method_t mth = HTTP_METHOD_POST;
    if      (strcasecmp(method, "GET")    == 0) mth = HTTP_METHOD_GET;
    else if (strcasecmp(method, "POST")   == 0) mth = HTTP_METHOD_POST;
    else if (strcasecmp(method, "PUT")    == 0) mth = HTTP_METHOD_PUT;
    else if (strcasecmp(method, "DELETE") == 0) mth = HTTP_METHOD_DELETE;
    else if (strcasecmp(method, "PATCH")  == 0) mth = HTTP_METHOD_PATCH;
    esp_http_client_set_method(h, mth);

    // Default Content-Type when sending a body and the user hasn't set one.
    bool has_content_type = false;
    if (cJSON_IsObject(headers)) {
        cJSON *hk = NULL;
        cJSON_ArrayForEach(hk, headers) {
            if (!hk->string || !cJSON_IsString(hk)) continue;
            if (strcasecmp(hk->string, "content-type") == 0) has_content_type = true;
            esp_http_client_set_header(h, hk->string, hk->valuestring);
        }
    }
    if (body && body_len > 0 && !has_content_type) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
    }
    if (body && body_len > 0) {
        esp_http_client_set_post_field(h, body, (int)body_len);
    }

    int rc = -1;
    if (!tls_lock_take(60000)) {
        esp_http_client_cleanup(h);
        return -2;
    }
    esp_err_t err = esp_http_client_perform(h);
    if (err == ESP_OK) {
        *status_out = esp_http_client_get_status_code(h);
        rc = 0;
    } else {
        ESP_LOGW(TAG, "http_request perform: %s", esp_err_to_name(err));
        rc = -3;
    }
    esp_http_client_cleanup(h);
    tls_lock_give();
    return rc;
}

int skill_tool_http_request(const char *args_json, char *out, size_t cap) {
    cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
    if (!args) {
        snprintf(out, cap, "{\"error\":\"bad_args\"}");
        return 0;
    }
    cJSON *jm = cJSON_GetObjectItem(args, "method");
    cJSON *ju = cJSON_GetObjectItem(args, "url");
    cJSON *jh = cJSON_GetObjectItem(args, "headers");
    cJSON *jb = cJSON_GetObjectItem(args, "body");

    if (!cJSON_IsString(jm) || !cJSON_IsString(ju) || !ju->valuestring[0]) {
        snprintf(out, cap, "{\"error\":\"missing_method_or_url\"}");
        cJSON_Delete(args);
        return 0;
    }

    char err[80] = {0};
    char *url = substitute_vars(ju->valuestring, err, sizeof(err));
    if (!url) {
        snprintf(out, cap, "{\"error\":\"%s\"}", err);
        cJSON_Delete(args);
        return 0;
    }

    if (cJSON_IsObject(jh)) {
        if (!substitute_in_json(jh, err, sizeof(err))) {
            free(url);
            cJSON_Delete(args);
            snprintf(out, cap, "{\"error\":\"%s\"}", err);
            return 0;
        }
    }

    char *body_str = NULL;
    size_t body_len = 0;
    if (cJSON_IsString(jb)) {
        body_str = substitute_vars(jb->valuestring, err, sizeof(err));
        if (!body_str) {
            free(url);
            cJSON_Delete(args);
            snprintf(out, cap, "{\"error\":\"%s\"}", err);
            return 0;
        }
        body_len = strlen(body_str);
    } else if (cJSON_IsObject(jb) || cJSON_IsArray(jb)) {
        if (!substitute_in_json(jb, err, sizeof(err))) {
            free(url);
            cJSON_Delete(args);
            snprintf(out, cap, "{\"error\":\"%s\"}", err);
            return 0;
        }
        body_str = cJSON_PrintUnformatted(jb);
        body_len = body_str ? strlen(body_str) : 0;
    }

    char *rsp = malloc(HTTP_RSP_BUF);
    if (!rsp) {
        free(url);
        if (body_str) free(body_str);
        cJSON_Delete(args);
        snprintf(out, cap, "{\"error\":\"oom\"}");
        return 0;
    }
    int status = 0;
    int rc = do_http_request(jm->valuestring, url, jh,
                             body_str, body_len,
                             rsp, HTTP_RSP_BUF, &status);
    free(url);
    if (body_str) free(body_str);
    cJSON_Delete(args);

    if (rc != 0) {
        snprintf(out, cap, "{\"error\":\"transport\",\"detail\":%d}", rc);
        free(rsp);
        return 0;
    }

    // Build response: {"status": <n>, "body": "<escaped>"}
    size_t bcap = 256;
    char *resp_buf = malloc(bcap);
    if (!resp_buf) { free(rsp); snprintf(out, cap, "{\"error\":\"oom\"}"); return 0; }
    int n = snprintf(resp_buf, bcap, "{\"status\":%d,\"body\":\"", status);
    size_t blen = (size_t)n;
    append_json_escaped(&resp_buf, &blen, &bcap, rsp, strlen(rsp));
    if (blen + 3 < bcap) {
        resp_buf[blen++] = '"';
        resp_buf[blen++] = '}';
        resp_buf[blen]   = '\0';
    }
    free(rsp);
    strlcpy(out, resp_buf, cap);
    free(resp_buf);
    return 0;
}

// ---------------------------------------------------------------------------
// x402_pay handler
// ---------------------------------------------------------------------------
//
// Args:
//   {
//     "payment_required": { ...the accepts[0] object from a 402 response... },
//     "memo":  "optional override; default is `<service> Order: <order_number>`"
//   }
//
// Builds: USDC TransferChecked + memo (optional) + ComputeBudget,
// fee-payer = payment_required.extra.feePayer (PayAI's wallet, not ours).
// Posts the signed v1 payload to <facilitator>/verify and /settle.
//
// Response:
//   { "ok": true,  "signature": "<base58>" }
//   { "ok": false, "error": "<message>" }
// ---------------------------------------------------------------------------

static const cJSON *get_extra(const cJSON *pr, const char *key) {
    const cJSON *e = cJSON_GetObjectItem(pr, "extra");
    if (!cJSON_IsObject(e)) return NULL;
    return cJSON_GetObjectItem(e, key);
}

// Build the v0 message bytes for: ComputeBudget(limit), ComputeBudget(price),
// SPL TransferChecked, [optional] Memo.
static int build_x402_message(const uint8_t fee_payer[32],
                              const uint8_t wallet[32],
                              const uint8_t source_ata[32],
                              const uint8_t dest_ata[32],
                              const uint8_t mint[32],
                              const uint8_t blockhash[32],
                              uint64_t amount_atomic,
                              uint8_t  decimals,
                              const char *memo, size_t memo_len,
                              uint8_t *out, size_t out_cap) {
    uint8_t token_program[32], compute_budget[32], memo_program[32];
    if (base58_decode(TOKEN_PROGRAM_B58,  token_program,  32) != 32) return -1;
    if (base58_decode(COMPUTE_BUDGET_B58, compute_budget, 32) != 32) return -1;
    if (memo_len > 0 &&
        base58_decode(MEMO_PROGRAM_B58, memo_program, 32) != 32) return -1;

    bool has_memo = memo_len > 0;
    uint8_t key_count = has_memo ? 8 : 7;
    uint8_t ix_count  = has_memo ? 4 : 3;
    uint8_t ro_unsigned = has_memo ? 4 : 3;

    wc_t w = { .buf = out, .cap = out_cap };
    wc_u8(&w, 0x80);                  // v0 prefix
    wc_u8(&w, 2);                     // num required signatures
    wc_u8(&w, 0);                     // num readonly signed
    wc_u8(&w, ro_unsigned);

    wc_shortvec(&w, key_count);
    wc_bytes(&w, fee_payer,      32);
    wc_bytes(&w, wallet,         32);
    wc_bytes(&w, source_ata,     32);
    wc_bytes(&w, dest_ata,       32);
    wc_bytes(&w, mint,           32);
    wc_bytes(&w, token_program,  32);
    wc_bytes(&w, compute_budget, 32);
    if (has_memo) wc_bytes(&w, memo_program, 32);

    wc_bytes(&w, blockhash, 32);
    wc_shortvec(&w, ix_count);

    // ComputeBudget SetCULimit (program=6, no accounts, [0x02][u32])
    wc_u8(&w, 6);
    wc_shortvec(&w, 0);
    wc_shortvec(&w, 5);
    wc_u8(&w, 0x02);
    wc_u32_le(&w, 30000);
    // ComputeBudget SetCUPrice (program=6, no accounts, [0x03][u64])
    wc_u8(&w, 6);
    wc_shortvec(&w, 0);
    wc_shortvec(&w, 9);
    wc_u8(&w, 0x03);
    wc_u64_le(&w, 1);
    // SPL TransferChecked (program=5, accounts={src,mint,dest,owner})
    wc_u8(&w, 5);
    wc_shortvec(&w, 4);
    wc_u8(&w, 2);
    wc_u8(&w, 4);
    wc_u8(&w, 3);
    wc_u8(&w, 1);
    wc_shortvec(&w, 10);
    wc_u8(&w, 0x0C);
    wc_u64_le(&w, amount_atomic);
    wc_u8(&w, decimals);

    if (has_memo) {
        // Memo (program=7, no accounts, data=memo bytes)
        wc_u8(&w, 7);
        wc_shortvec(&w, 0);
        wc_shortvec(&w, (uint16_t)memo_len);
        wc_bytes(&w, (const uint8_t *)memo, memo_len);
    }

    wc_shortvec(&w, 0);  // address-table lookups: none
    return w.overflow ? -1 : (int)w.len;
}

static int build_signed_tx_b64_with_memo(const cJSON *pr,
                                         const char *memo_override,
                                         char *out_b64, size_t out_cap,
                                         char *err, size_t err_cap) {
    const cJSON *pay_to_j = cJSON_GetObjectItem(pr, "payTo");
    const cJSON *amount_j = cJSON_GetObjectItem(pr, "maxAmountRequired");
    const cJSON *asset_j  = cJSON_GetObjectItem(pr, "asset");
    if (!cJSON_IsString(pay_to_j) || !cJSON_IsString(asset_j)) {
        snprintf(err, err_cap, "missing_payTo_or_asset");
        return -1;
    }
    if (strcmp(asset_j->valuestring, USDC_MINT_B58) != 0) {
        snprintf(err, err_cap, "unsupported_asset");
        return -1;
    }
    uint64_t amount = 0;
    if (cJSON_IsString(amount_j) && amount_j->valuestring) {
        amount = strtoull(amount_j->valuestring, NULL, 10);
    } else if (cJSON_IsNumber(amount_j)) {
        amount = (uint64_t)amount_j->valuedouble;
    }
    if (amount == 0) {
        snprintf(err, err_cap, "bad_amount");
        return -1;
    }

    const cJSON *fee_payer_j = get_extra(pr, "feePayer");
    if (!cJSON_IsString(fee_payer_j) || !fee_payer_j->valuestring) {
        snprintf(err, err_cap, "missing_feePayer");
        return -1;
    }

    // Decode 32-byte pubkeys.
    uint8_t fee_payer_bytes[32], mint_bytes[32];
    if (base58_decode(fee_payer_j->valuestring, fee_payer_bytes, 32) != 32) {
        snprintf(err, err_cap, "bad_feePayer");
        return -1;
    }
    if (base58_decode(USDC_MINT_B58, mint_bytes, 32) != 32) {
        snprintf(err, err_cap, "bad_mint");
        return -1;
    }
    const uint8_t *wallet_bytes = wallet_pubkey_bytes();
    if (!wallet_bytes) {
        snprintf(err, err_cap, "no_wallet");
        return -1;
    }

    // Source ATA = our USDC ATA. Wallet may have it cached; otherwise resolve.
    char source_ata_b58[64];
    const char *cached = wallet_usdc_ata();
    if (cached && cached[0]) {
        strlcpy(source_ata_b58, cached, sizeof(source_ata_b58));
    } else if (!x402_fetch_usdc_ata(wallet_pubkey(),
                                    source_ata_b58, sizeof(source_ata_b58))) {
        snprintf(err, err_cap, "no_source_ata");
        return -1;
    }
    uint8_t source_ata_bytes[32];
    if (base58_decode(source_ata_b58, source_ata_bytes, 32) != 32) {
        snprintf(err, err_cap, "bad_source_ata");
        return -1;
    }

    // Destination ATA — resolve via RPC for the recipient's USDC account.
    char dest_ata_b58[64];
    if (!x402_fetch_usdc_ata(pay_to_j->valuestring,
                             dest_ata_b58, sizeof(dest_ata_b58))) {
        snprintf(err, err_cap, "no_dest_ata");
        return -1;
    }
    uint8_t dest_ata_bytes[32];
    if (base58_decode(dest_ata_b58, dest_ata_bytes, 32) != 32) {
        snprintf(err, err_cap, "bad_dest_ata");
        return -1;
    }

    // Fresh blockhash.
    uint8_t blockhash[32];
    if (!x402_fetch_recent_blockhash(blockhash)) {
        snprintf(err, err_cap, "no_blockhash");
        return -1;
    }

    // Memo string: prefer override; else build from order_number.
    char memo_buf[160] = {0};
    const char *memo = NULL;
    size_t memo_len = 0;
    if (memo_override && memo_override[0]) {
        memo = memo_override;
        memo_len = strlen(memo);
    } else {
        const cJSON *order_j = get_extra(pr, "order_number");
        if (cJSON_IsString(order_j) && order_j->valuestring) {
            // Default service-name from extra.name if present.
            const cJSON *name_j = get_extra(pr, "name");
            const char *svc_name = (cJSON_IsString(name_j) && name_j->valuestring)
                                       ? name_j->valuestring
                                       : "Order";
            snprintf(memo_buf, sizeof(memo_buf), "%s: %s",
                     svc_name, order_j->valuestring);
            memo     = memo_buf;
            memo_len = strlen(memo_buf);
        }
    }
    if (memo_len > 120) memo_len = 120;  // keep tx short

    // Build the v0 message.
    uint8_t msg[512];
    int msg_len = build_x402_message(fee_payer_bytes,
                                     wallet_bytes,
                                     source_ata_bytes,
                                     dest_ata_bytes,
                                     mint_bytes,
                                     blockhash,
                                     amount, 6,
                                     memo, memo_len,
                                     msg, sizeof(msg));
    if (msg_len < 0) {
        snprintf(err, err_cap, "msg_overflow");
        return -1;
    }

    // Sign.
    uint8_t sig[64];
    if (!wallet_sign(msg, (size_t)msg_len, sig)) {
        snprintf(err, err_cap, "sign_failed");
        return -1;
    }

    // Wrap into VersionedTransaction (sigs prefix + msg).
    uint8_t tx[1 + 64 + 64 + 512];
    wc_t tw = { .buf = tx, .cap = sizeof(tx) };
    wc_shortvec(&tw, 2);  // 2 signatures
    uint8_t zero_sig[64] = {0};
    wc_bytes(&tw, zero_sig, 64);
    wc_bytes(&tw, sig, 64);
    wc_bytes(&tw, msg, (size_t)msg_len);
    if (tw.overflow) {
        snprintf(err, err_cap, "tx_overflow");
        return -1;
    }

    // Base64-encode into out_b64.
    size_t written = 0;
    int rc = mbedtls_base64_encode((uint8_t *)out_b64, out_cap, &written,
                                   tw.buf, tw.len);
    if (rc != 0 || written >= out_cap) {
        snprintf(err, err_cap, "b64_failed");
        return -1;
    }
    out_b64[written] = '\0';
    return (int)written;
}

// POST to <facilitator>/<endpoint> with the v1 paymentPayload + paymentRequirements
// body. Captures response body. Returns HTTP status, or -1 on transport error.
static int post_facilitator(const char *facilitator, const char *endpoint,
                            const char *payload_json,
                            char *rsp, size_t rsp_cap) {
    char url[256];
    snprintf(url, sizeof(url), "%s/%s", facilitator, endpoint);

    body_capture_t bc = { .buf = rsp, .cap = rsp_cap };
    rsp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        .event_handler     = body_event_cb,
        .user_data         = &bc,
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
        .method            = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return -1;
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, payload_json, (int)strlen(payload_json));

    int code = -1;
    if (!tls_lock_take(60000)) {
        esp_http_client_cleanup(h);
        return -1;
    }
    esp_err_t err = esp_http_client_perform(h);
    if (err == ESP_OK) {
        code = esp_http_client_get_status_code(h);
    } else {
        ESP_LOGW(TAG, "facilitator %s perform: %s", endpoint, esp_err_to_name(err));
    }
    esp_http_client_cleanup(h);
    tls_lock_give();
    return code;
}

int skill_tool_x402_pay(const char *args_json, char *out, size_t cap) {
    cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
    if (!args) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"bad_args\"}");
        return 0;
    }
    cJSON *pr = cJSON_GetObjectItem(args, "payment_required");
    cJSON *memo_j = cJSON_GetObjectItem(args, "memo");
    if (!cJSON_IsObject(pr)) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"missing_payment_required\"}");
        cJSON_Delete(args);
        return 0;
    }

    // Some 402 payloads are wrapped in {"accepts":[...]}; auto-unwrap.
    cJSON *accepts = cJSON_GetObjectItem(pr, "accepts");
    if (cJSON_IsArray(accepts) && cJSON_GetArraySize(accepts) > 0) {
        pr = cJSON_GetArrayItem(accepts, 0);
    }

    char err[80] = {0};
    char *signed_b64 = malloc(2048);
    if (!signed_b64) {
        cJSON_Delete(args);
        snprintf(out, cap, "{\"ok\":false,\"error\":\"oom\"}");
        return 0;
    }
    int b64_len = build_signed_tx_b64_with_memo(
        pr,
        cJSON_IsString(memo_j) ? memo_j->valuestring : NULL,
        signed_b64, 2048,
        err, sizeof(err));
    if (b64_len <= 0) {
        free(signed_b64);
        cJSON_Delete(args);
        snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", err);
        return 0;
    }

    // Build x402 v1 payload + paymentRequirements (mirrors SP3ND example).
    cJSON *v1payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(v1payload, "x402Version", 1);
    cJSON_AddStringToObject(v1payload, "scheme",      "exact");
    cJSON_AddStringToObject(v1payload, "network",     "solana");
    cJSON *p = cJSON_AddObjectToObject(v1payload, "payload");
    cJSON_AddStringToObject(p, "transaction", signed_b64);
    free(signed_b64);

    // v1 requirements: copy payTo/maxAmountRequired/resource/asset/extra
    // from pr, set scheme/network to v1 values.
    cJSON *v1req = cJSON_CreateObject();
    cJSON_AddStringToObject(v1req, "scheme",  "exact");
    cJSON_AddStringToObject(v1req, "network", "solana");
    const cJSON *amt = cJSON_GetObjectItem(pr, "maxAmountRequired");
    if (cJSON_IsString(amt)) {
        cJSON_AddStringToObject(v1req, "maxAmountRequired", amt->valuestring);
        cJSON_AddStringToObject(v1req, "amount",            amt->valuestring);
    }
    const cJSON *res = cJSON_GetObjectItem(pr, "resource");
    if (cJSON_IsString(res)) cJSON_AddStringToObject(v1req, "resource", res->valuestring);
    const cJSON *pay_to = cJSON_GetObjectItem(pr, "payTo");
    if (cJSON_IsString(pay_to)) cJSON_AddStringToObject(v1req, "payTo", pay_to->valuestring);
    const cJSON *to = cJSON_GetObjectItem(pr, "maxTimeoutSeconds");
    if (cJSON_IsNumber(to)) cJSON_AddNumberToObject(v1req, "maxTimeoutSeconds", to->valuedouble);
    const cJSON *asset = cJSON_GetObjectItem(pr, "asset");
    if (cJSON_IsString(asset)) cJSON_AddStringToObject(v1req, "asset", asset->valuestring);
    const cJSON *extra = cJSON_GetObjectItem(pr, "extra");
    if (cJSON_IsObject(extra)) cJSON_AddItemReferenceToObject(v1req, "extra", (cJSON *)extra);

    // Determine facilitator URL.
    const cJSON *fac_j = get_extra(pr, "facilitator");
    const char *facilitator = (cJSON_IsString(fac_j) && fac_j->valuestring && fac_j->valuestring[0])
                                  ? fac_j->valuestring
                                  : X402_FACILITATOR_DEFAULT;

    cJSON *envelope = cJSON_CreateObject();
    cJSON_AddItemToObject(envelope, "paymentPayload",      v1payload);
    cJSON_AddItemToObject(envelope, "paymentRequirements", v1req);
    char *envelope_str = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    cJSON_Delete(args);
    if (!envelope_str) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"oom\"}");
        return 0;
    }

    char rsp[1024];
    int verify_status = post_facilitator(facilitator, "verify", envelope_str, rsp, sizeof(rsp));
    if (verify_status != 200) {
        free(envelope_str);
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"verify_status_%d\",\"detail\":\"%.256s\"}",
                 verify_status, rsp);
        return 0;
    }

    int settle_status = post_facilitator(facilitator, "settle", envelope_str, rsp, sizeof(rsp));
    free(envelope_str);
    if (settle_status != 200) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"settle_status_%d\",\"detail\":\"%.256s\"}",
                 settle_status, rsp);
        return 0;
    }

    // Pull the on-chain signature from the settle response if present.
    cJSON *sr = cJSON_Parse(rsp);
    char sig[96] = {0};
    if (sr) {
        const cJSON *txid = cJSON_GetObjectItem(sr, "transaction");
        if (!cJSON_IsString(txid)) txid = cJSON_GetObjectItem(sr, "signature");
        if (cJSON_IsString(txid) && txid->valuestring) {
            strlcpy(sig, txid->valuestring, sizeof(sig));
        }
        cJSON_Delete(sr);
    }
    if (sig[0]) {
        snprintf(out, cap, "{\"ok\":true,\"signature\":\"%s\"}", sig);
    } else {
        snprintf(out, cap, "{\"ok\":true,\"signature\":\"\"}");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// secret_get / secret_set / solana_get_pubkey
// ---------------------------------------------------------------------------
int skill_tool_secret_get(const char *args_json, char *out, size_t cap) {
    cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
    cJSON *js = args ? cJSON_GetObjectItem(args, "skill") : NULL;
    cJSON *jn = args ? cJSON_GetObjectItem(args, "name")  : NULL;
    if (!cJSON_IsString(js) || !cJSON_IsString(jn)) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"bad_args\"}");
        if (args) cJSON_Delete(args);
        return 0;
    }
    char value[SKILL_CRED_VALUE_MAX] = {0};
    int n = skill_store_get_cred(js->valuestring, jn->valuestring,
                                 value, sizeof(value));
    cJSON_Delete(args);
    if (n <= 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"unset\"}");
        return 0;
    }
    // Emit {"ok":true,"value":"<escaped>"}
    size_t bcap = 256;
    char *resp = malloc(bcap);
    if (!resp) { snprintf(out, cap, "{\"ok\":false,\"error\":\"oom\"}"); return 0; }
    int hn = snprintf(resp, bcap, "{\"ok\":true,\"value\":\"");
    size_t blen = (size_t)hn;
    append_json_escaped(&resp, &blen, &bcap, value, strlen(value));
    if (blen + 3 < bcap) {
        resp[blen++] = '"';
        resp[blen++] = '}';
        resp[blen]   = '\0';
    }
    strlcpy(out, resp, cap);
    free(resp);
    return 0;
}

int skill_tool_secret_set(const char *args_json, char *out, size_t cap) {
    cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
    cJSON *js = args ? cJSON_GetObjectItem(args, "skill") : NULL;
    cJSON *jn = args ? cJSON_GetObjectItem(args, "name")  : NULL;
    cJSON *jv = args ? cJSON_GetObjectItem(args, "value") : NULL;
    if (!cJSON_IsString(js) || !cJSON_IsString(jn) || !cJSON_IsString(jv)) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"bad_args\"}");
        if (args) cJSON_Delete(args);
        return 0;
    }
    esp_err_t rc = skill_store_set_cred(js->valuestring,
                                        jn->valuestring,
                                        jv->valuestring);
    cJSON_Delete(args);
    if (rc != ESP_OK) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"set_failed\"}");
        return 0;
    }
    snprintf(out, cap, "{\"ok\":true}");
    return 0;
}

int skill_tool_solana_get_pubkey(const char *args_json, char *out, size_t cap) {
    (void)args_json;
    const char *pk = wallet_pubkey();
    if (!pk || !pk[0]) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"no_wallet\"}");
        return 0;
    }
    snprintf(out, cap, "{\"ok\":true,\"pubkey\":\"%s\"}", pk);
    return 0;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
bool skill_tools_is_skill_tool(const char *name) {
    if (!name) return false;
    return strcmp(name, "http_request")      == 0 ||
           strcmp(name, "x402_pay")          == 0 ||
           strcmp(name, "secret_get")        == 0 ||
           strcmp(name, "secret_set")        == 0 ||
           strcmp(name, "solana_get_pubkey") == 0;
}

int skill_tools_dispatch(const char *name, const char *args_json,
                         char *out, size_t cap) {
    if (!name || !out || cap == 0) return -1;
    if (strcmp(name, "http_request")      == 0) return skill_tool_http_request(args_json, out, cap);
    if (strcmp(name, "x402_pay")          == 0) return skill_tool_x402_pay(args_json, out, cap);
    if (strcmp(name, "secret_get")        == 0) return skill_tool_secret_get(args_json, out, cap);
    if (strcmp(name, "secret_set")        == 0) return skill_tool_secret_set(args_json, out, cap);
    if (strcmp(name, "solana_get_pubkey") == 0) return skill_tool_solana_get_pubkey(args_json, out, cap);
    snprintf(out, cap, "{\"error\":\"unknown_tool:%s\"}", name);
    return -1;
}
