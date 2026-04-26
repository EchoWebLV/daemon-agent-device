// ---------------------------------------------------------------------------
//  Solana wallet. See wallet.h.
//
//  The key comes from secrets.h as a base58 string. Accepting either form:
//    • 32 bytes  → pubkey only (read-only wallet)
//    • 64 bytes  → Phantom-style [seed || pubkey] (signing-capable, later)
//  For phase 3 we only derive/display the pubkey + poll balances. Phase 4
//  will vendor Ed25519 and flip wallet_can_sign() on when the key is 64B.
//
//  HTTP: esp_http_client + crt_bundle. JSON: cJSON. Refreshes run on a
//  background FreeRTOS task so /say and the UI never stall.
// ---------------------------------------------------------------------------
#include "wallet.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "agent_pda.h"
#include "base58.h"
#include "ed25519.h"
#include "refill.h"
#include "secrets.h"
#include "wifi_sta.h"

static const char *TAG = "wallet";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char *USDC_MINT        = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
static const char *TOKEN_PROGRAM_ID = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
static const char *SOLANA_PUBLIC_RPC = "https://api.mainnet-beta.solana.com";

// Responses from Helius for getTokenAccountsByOwner can be 5–10 KB with
// a handful of tokens and jsonParsed encoding. Leave headroom.
#define WALLET_RSP_MAX 16384

// Known mint → symbol hints so the AI prompt reads "12,000 USDC" rather
// than "12,000 EPjFW…". Extend as the cabinet grows.
typedef struct { const char *mint; const char *sym; uint8_t dec; } known_mint_t;
static const known_mint_t KNOWN_MINTS[] = {
    { "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v", "USDC",  6 },
    { "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB", "USDT",  6 },
    { "mSoLzYCxHdYgdzU16g5QSh3i5K3z3KZK7ytfqcJm7So",  "mSOL",  9 },
    { "7dHbWXmci3dT8UFYWYZweBLXgycu7Y3iL6trKn1Y7ARj", "stSOL", 9 },
    { "jtojtomepa8beP8AuQc6eXt5FriJwfFMwQx2v2f9mCL",  "JTO",   9 },
    { "JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN",  "JUP",   6 },
    { "EKpQGSJtjMFqKZ9KQanSqYXRcF8fBopzLHYxdM65zcjm", "WIF",   6 },
    { "DezXAZ8z7PnrnRJjz3wXBoRgixCa6xjnB7YaB1pPB263", "BONK",  5 },
};
static const size_t N_KNOWN_MINTS = sizeof(KNOWN_MINTS) / sizeof(KNOWN_MINTS[0]);

static const char *symbol_for_mint(const char *mint) {
    for (size_t i = 0; i < N_KNOWN_MINTS; ++i) {
        if (strcmp(KNOWN_MINTS[i].mint, mint) == 0) return KNOWN_MINTS[i].sym;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool    s_ok                    = false;
static uint8_t s_secret[64]            = {0};     // full 64 or pubkey-only 32
static size_t  s_secret_len            = 0;       // 32 or 64
static uint8_t s_pubkey_bytes[32]      = {0};
static char    s_pubkey[45]            = {0};     // base58, NUL-terminated

// Recovery-authority pubkey, decoded from secrets.h's OWNER_PUBKEY at
// wallet_begin() time. Used to derive the on-chain agent_root PDA. The
// device never needs the matching seed — only the pubkey.
static bool    s_owner_ok              = false;
static uint8_t s_owner_pubkey_bytes[32] = {0};
static char    s_owner_pubkey[45]      = {0};

// Cached PDAs / ATAs — derived at wallet_begin() once we have both the
// owner key and the device key in hand. ata derivation runs ~8 sha256
// rounds on average so caching matters for the x402 hot path.
static bool    s_chain_ok              = false;
static uint8_t s_vault_pda_bytes[32]   = {0};
static char    s_vault_pda[45]         = {0};
static uint8_t s_vault_usdc_ata_bytes[32] = {0};
static char    s_vault_usdc_ata[45]    = {0};
static uint8_t s_device_usdc_ata_bytes[32] = {0};
static char    s_device_usdc_ata_b58[45] = {0};

// orlp/ed25519 signs with an "expanded" 64-byte private key that is *not*
// Solana's `[seed || pub]` layout — it's `[a || prefix]` where both halves
// come from SHA-512(seed). We derive it at wallet_begin() time so every
// sign() call is a pure curve op without re-hashing the seed.
static uint8_t s_sign_priv[64]         = {0};
static bool    s_sign_ready            = false;

static double           s_sol_balance  = 0.0;
static token_holding_t  s_tokens[WALLET_MAX_TOKENS];
static size_t           s_tokens_n     = 0;
static char             s_usdc_ata[WALLET_MINT_MAX] = {0};
static int64_t          s_last_refresh_us = 0;    // esp_timer_get_time()

// Previous snapshot — used at the tail of wallet_refresh() to detect
// positive balance deltas and fire s_incoming_cb. s_prev_seeded stays false
// until the very first refresh completes so the starting balance never
// announces itself as "received".
static double               s_prev_sol      = 0.0;
static token_holding_t      s_prev_tokens[WALLET_MAX_TOKENS];
static size_t               s_prev_tokens_n = 0;
static bool                 s_prev_seeded   = false;
static wallet_incoming_cb_t s_incoming_cb   = NULL;

// Minimum positive delta to announce. Tuned so refresh-to-refresh rounding
// or tiny reward dust doesn't trigger the AI/TTS path (each announcement
// costs a chat round-trip and therefore a USDC micropayment).
static const double INCOMING_SOL_MIN  = 0.01;
static const double INCOMING_USDC_MIN = 0.10;
static const double INCOMING_SPL_MIN  = 0.10;

// Endpoint buffer sized for https + helius domain + ?api-key=...
static char             s_rpc_url[160] = {0};

// Background task plumbing
static TaskHandle_t      s_task    = NULL;
static SemaphoreHandle_t s_trigger = NULL;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void wallet_rpc_url(char *out, size_t cap) {
    if (!out || cap == 0) return;
    const char *key = HELIUS_API_KEY;
    if (key && key[0] && strncmp(key, "PASTE-", 6) != 0 && strlen(key) >= 10) {
        snprintf(out, cap, "https://mainnet.helius-rpc.com/?api-key=%s", key);
    } else {
        snprintf(out, cap, "%s", SOLANA_PUBLIC_RPC);
    }
}

bool wallet_begin(void) {
    const char *raw = SOLANA_KEY;
    if (!raw || !raw[0] || strncmp(raw, "PASTE-", 6) == 0) {
        ESP_LOGI(TAG, "no key configured — wallet disabled");
        s_ok = false;
        return false;
    }

    // Worst case a 64-byte blob encodes to ~88 chars of base58.
    uint8_t decoded[64];
    int n = base58_decode(raw, decoded, sizeof(decoded));
    if (n != 32 && n != 64) {
        ESP_LOGE(TAG, "key decode yielded %d bytes (want 32 or 64)", n);
        s_ok = false;
        return false;
    }
    memcpy(s_secret, decoded, (size_t)n);
    s_secret_len = (size_t)n;

    // For 64-byte Phantom-style export, the pubkey is the tail 32 bytes.
    const uint8_t *pub = (n == 64) ? (s_secret + 32) : s_secret;
    memcpy(s_pubkey_bytes, pub, 32);

    if (base58_encode(pub, 32, s_pubkey, sizeof(s_pubkey)) < 0) {
        ESP_LOGE(TAG, "pubkey encode failed");
        s_ok = false;
        return false;
    }

    // For signing: expand the seed (s_secret[0..32]) into orlp's 64-byte
    // private-key form and verify the derived pubkey matches the one
    // Phantom stored. Mismatch means the blob isn't a valid Solana export.
    s_sign_ready = false;
    if (s_secret_len == 64) {
        uint8_t derived_pub[32];
        ed25519_create_keypair(derived_pub, s_sign_priv, s_secret);
        if (memcmp(derived_pub, s_pubkey_bytes, 32) == 0) {
            s_sign_ready = true;
        } else {
            ESP_LOGW(TAG, "seed/pubkey mismatch — signing disabled");
        }
    }

    // Decode OWNER_PUBKEY (recovery_authority) from secrets.h. This is
    // separate from the device key — it's the user's CLI / Phantom pubkey
    // that owns the on-chain agent_root PDA. Optional: if missing or still
    // the placeholder, the wallet still works for read-only ops; only
    // vault_execute paths need it.
    s_owner_ok = false;
    s_chain_ok = false;
    const char *owner_raw = OWNER_PUBKEY;
    if (owner_raw && owner_raw[0] && strncmp(owner_raw, "REPLACE_ME", 10) != 0) {
        uint8_t obuf[32];
        int on = base58_decode(owner_raw, obuf, sizeof obuf);
        if (on == 32) {
            memcpy(s_owner_pubkey_bytes, obuf, 32);
            snprintf(s_owner_pubkey, sizeof s_owner_pubkey, "%s", owner_raw);
            s_owner_ok = true;
            ESP_LOGI(TAG, "owner_pubkey %s", s_owner_pubkey);

            // Cache the agent_root → vault → vault_usdc_ata chain plus the
            // device's own USDC ATA (used as the spending-account in the
            // x402 dual-instruction tx). None of these depend on network
            // state — pure cryptographic derivations from owner + device +
            // USDC mint constant.
            uint8_t agent_root[32], usdc_mint[32];
            if (base58_decode(USDC_MINT, usdc_mint, 32) == 32 &&
                agent_pda_derive_root(s_owner_pubkey_bytes, agent_root) >= 0 &&
                agent_pda_derive_vault(agent_root, s_vault_pda_bytes) >= 0 &&
                agent_pda_derive_ata(s_vault_pda_bytes, usdc_mint,
                                     s_vault_usdc_ata_bytes) >= 0 &&
                agent_pda_derive_ata(s_pubkey_bytes, usdc_mint,
                                     s_device_usdc_ata_bytes) >= 0)
            {
                base58_encode(s_vault_pda_bytes, 32, s_vault_pda, sizeof s_vault_pda);
                base58_encode(s_vault_usdc_ata_bytes, 32, s_vault_usdc_ata, sizeof s_vault_usdc_ata);
                base58_encode(s_device_usdc_ata_bytes, 32, s_device_usdc_ata_b58, sizeof s_device_usdc_ata_b58);
                s_chain_ok = true;
                ESP_LOGI(TAG, "vault           %s", s_vault_pda);
                ESP_LOGI(TAG, "vault_usdc_ata  %s", s_vault_usdc_ata);
                ESP_LOGI(TAG, "device_usdc_ata %s", s_device_usdc_ata_b58);
            } else {
                ESP_LOGW(TAG, "vault PDA derivation failed");
            }
        } else {
            ESP_LOGW(TAG, "OWNER_PUBKEY decode failed (%d bytes, want 32)", on);
        }
    } else {
        ESP_LOGI(TAG, "OWNER_PUBKEY not configured — vault paths disabled");
    }

    wallet_rpc_url(s_rpc_url, sizeof(s_rpc_url));
    s_ok = true;
    ESP_LOGI(TAG, "address %s (key %d bytes, can_sign=%d)",
             s_pubkey, (int)s_secret_len, (int)wallet_can_sign());
    return true;
}

bool wallet_can_sign(void) {
    return s_ok && s_sign_ready;
}

bool wallet_sign(const uint8_t *data, size_t len, uint8_t sig_out[64]) {
    if (!wallet_can_sign() || !data || !sig_out) return false;
    ed25519_sign(sig_out, data, len, s_pubkey_bytes, s_sign_priv);
    return true;
}

const char *wallet_pubkey(void)                 { return s_ok ? s_pubkey : ""; }
const uint8_t *wallet_pubkey_bytes(void)        { return s_ok ? s_pubkey_bytes : NULL; }

const uint8_t *wallet_owner_pubkey_bytes(void)  { return s_owner_ok ? s_owner_pubkey_bytes : NULL; }
const char    *wallet_owner_pubkey(void)        { return s_owner_ok ? s_owner_pubkey : ""; }

const uint8_t *wallet_vault_pda_bytes(void)        { return s_chain_ok ? s_vault_pda_bytes : NULL; }
const char    *wallet_vault_pda(void)              { return s_chain_ok ? s_vault_pda : ""; }
const uint8_t *wallet_vault_usdc_ata_bytes(void)   { return s_chain_ok ? s_vault_usdc_ata_bytes : NULL; }
const char    *wallet_vault_usdc_ata(void)         { return s_chain_ok ? s_vault_usdc_ata : ""; }
const uint8_t *wallet_device_usdc_ata_bytes(void)  { return s_chain_ok ? s_device_usdc_ata_bytes : NULL; }
const char    *wallet_device_usdc_ata(void)        { return s_chain_ok ? s_device_usdc_ata_b58 : ""; }

double wallet_sol_balance(void)                 { return s_sol_balance; }

void wallet_tokens(const token_holding_t **out, size_t *count) {
    if (out)   *out   = s_tokens;
    if (count) *count = s_tokens_n;
}

double wallet_usdc_amount(void) {
    for (size_t i = 0; i < s_tokens_n; ++i) {
        if (strcmp(s_tokens[i].mint, USDC_MINT) == 0) return s_tokens[i].amount;
    }
    return 0.0;
}

const char *wallet_usdc_ata(void)               { return s_usdc_ata; }

uint32_t wallet_last_refresh_age_ms(void) {
    if (s_last_refresh_us == 0) return UINT32_MAX;
    int64_t now = esp_timer_get_time();
    int64_t d   = (now - s_last_refresh_us) / 1000;
    if (d < 0) return 0;
    if (d > (int64_t)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)d;
}

void wallet_usdc_display_string(char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!s_ok) { snprintf(out, cap, "USDC ---"); return; }
    double u = wallet_usdc_amount();
    if      (u >= 10000) snprintf(out, cap, "USDC %.0f", u);
    else if (u >= 100)   snprintf(out, cap, "USDC %.1f", u);
    else if (u >= 1)     snprintf(out, cap, "USDC %.2f", u);
    else if (u >  0)     snprintf(out, cap, "USDC %.4f", u);
    else                 snprintf(out, cap, "USDC 0.00");
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} rsp_buf_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    rsp_buf_t *r = (rsp_buf_t *)evt->user_data;
    if (!r || !r->buf) return ESP_OK;
    size_t room = r->cap - 1 - r->len;
    if (room == 0) return ESP_OK;
    size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    memcpy(r->buf + r->len, evt->data, take);
    r->len += take;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

// Blocking JSON-RPC POST. Returns true if HTTP 200 and the response fit.
// Caller owns the destination buffer.
static bool rpc_call(const char *payload, char *out, size_t out_cap) {
    if (!wifi_sta_is_connected()) return false;
    rsp_buf_t rsp = { .buf = out, .cap = out_cap, .len = 0 };
    if (out_cap) out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = s_rpc_url,
        .method            = HTTP_METHOD_POST,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = on_http_event,
        .user_data         = &rsp,
        .timeout_ms        = 15000,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return false;

    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_post_field(http, payload, (int)strlen(payload));

    esp_err_t err  = esp_http_client_perform(http);
    int       code = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "perform: %s", esp_err_to_name(err));
        return false;
    }
    if (code != 200) {
        ESP_LOGW(TAG, "rpc HTTP %d", code);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Refresh primitives
// ---------------------------------------------------------------------------
// Heap-allocate the RPC response buffer from PSRAM — 16 KB is too big for
// the wallet task's stack, and the two refresh calls happen serially so we
// only need one at a time. PSRAM keeps internal RAM free for TLS handshakes.
static char *alloc_rpc_buf(void) {
    return (char *)heap_caps_malloc(WALLET_RSP_MAX, MALLOC_CAP_SPIRAM);
}

static void refresh_sol_balance(char *buf) {
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"getBalance\",\"params\":[\"%s\"]}",
             s_pubkey);
    if (!rpc_call(payload, buf, WALLET_RSP_MAX)) return;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *value  = result ? cJSON_GetObjectItemCaseSensitive(result, "value") : NULL;
    if (cJSON_IsNumber(value)) {
        // getBalance returns lamports as a u64; cJSON uses double internally
        // so we accept the implicit truncation — balances above 9e15 would
        // lose precision but that's well beyond the S3's wallet use case.
        s_sol_balance = value->valuedouble / 1e9;
    }
    cJSON_Delete(root);
}

static void refresh_tokens(char *buf) {
    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"getTokenAccountsByOwner\",\"params\":[\"%s\","
             "{\"programId\":\"%s\"},"
             "{\"encoding\":\"jsonParsed\"}]}",
             s_pubkey, TOKEN_PROGRAM_ID);
    if (!rpc_call(payload, buf, WALLET_RSP_MAX)) return;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *value  = result ? cJSON_GetObjectItemCaseSensitive(result, "value") : NULL;
    if (!cJSON_IsArray(value)) { cJSON_Delete(root); return; }

    // Build a fresh list; don't mutate s_tokens until we've fully parsed so
    // a bad mid-parse doesn't leave the UI looking at partial data.
    // Static because refresh_tokens() only runs on the single wallet task,
    // and the overshoot array is too big (~1.4 KB) for the task stack.
    static token_holding_t next[WALLET_MAX_TOKENS * 2];
    size_t                 next_n = 0;
    char                   found_usdc_ata[WALLET_MINT_MAX] = {0};

    cJSON *acct = NULL;
    cJSON_ArrayForEach(acct, value) {
        if (next_n >= sizeof(next) / sizeof(next[0])) break;
        const cJSON *pubkey = cJSON_GetObjectItemCaseSensitive(acct, "pubkey");
        const cJSON *data   = cJSON_GetObjectItemCaseSensitive(
                                  cJSON_GetObjectItemCaseSensitive(acct, "account"),
                                  "data");
        const cJSON *info   = cJSON_GetObjectItemCaseSensitive(
                                  cJSON_GetObjectItemCaseSensitive(data, "parsed"),
                                  "info");
        if (!info) continue;
        const cJSON *mint   = cJSON_GetObjectItemCaseSensitive(info, "mint");
        const cJSON *amt    = cJSON_GetObjectItemCaseSensitive(info, "tokenAmount");
        if (!cJSON_IsString(mint) || !cJSON_IsObject(amt)) continue;

        // Track the USDC ATA even with zero balance — we still need the
        // address at payment time and the RPC order isn't guaranteed.
        if (strcmp(mint->valuestring, USDC_MINT) == 0 &&
            cJSON_IsString(pubkey)) {
            strlcpy(found_usdc_ata, pubkey->valuestring,
                    sizeof(found_usdc_ata));
        }

        const cJSON *ui_amount = cJSON_GetObjectItemCaseSensitive(amt, "uiAmount");
        const cJSON *decimals  = cJSON_GetObjectItemCaseSensitive(amt, "decimals");
        double ui = cJSON_IsNumber(ui_amount) ? ui_amount->valuedouble : 0.0;
        if (ui <= 0.0) continue;

        token_holding_t *t = &next[next_n++];
        memset(t, 0, sizeof(*t));
        strlcpy(t->mint, mint->valuestring, sizeof(t->mint));
        strlcpy(t->symbol, symbol_for_mint(t->mint), sizeof(t->symbol));
        t->amount   = ui;
        t->decimals = (uint8_t)(cJSON_IsNumber(decimals) ? decimals->valueint : 0);
    }
    cJSON_Delete(root);

    // Sort by amount desc. Small N — bubble sort keeps it tiny.
    for (size_t i = 0; i < next_n; ++i) {
        for (size_t j = i + 1; j < next_n; ++j) {
            if (next[j].amount > next[i].amount) {
                token_holding_t tmp = next[i];
                next[i] = next[j];
                next[j] = tmp;
            }
        }
    }
    size_t keep = next_n < WALLET_MAX_TOKENS ? next_n : WALLET_MAX_TOKENS;
    memcpy(s_tokens, next, keep * sizeof(token_holding_t));
    s_tokens_n = keep;
    memcpy(s_usdc_ata, found_usdc_ata, sizeof(s_usdc_ata));
}

void wallet_set_incoming_cb(wallet_incoming_cb_t cb) { s_incoming_cb = cb; }

// Compare the freshly-refreshed balances against the previous snapshot and
// fire s_incoming_cb for each positive delta above the configured minimum.
// Tokens are matched by mint — a token newly appearing in the list counts
// as prev=0, so a fresh SPL arrival is announced as a full-balance delta.
static void detect_incoming(void) {
    if (!s_prev_seeded || !s_incoming_cb) return;

    double dsol = s_sol_balance - s_prev_sol;
    if (dsol > INCOMING_SOL_MIN) s_incoming_cb("SOL", dsol);

    for (size_t i = 0; i < s_tokens_n; ++i) {
        double prev = 0.0;
        for (size_t j = 0; j < s_prev_tokens_n; ++j) {
            if (strcmp(s_tokens[i].mint, s_prev_tokens[j].mint) == 0) {
                prev = s_prev_tokens[j].amount;
                break;
            }
        }
        double d = s_tokens[i].amount - prev;
        bool is_usdc = strcmp(s_tokens[i].mint, USDC_MINT) == 0;
        double thr   = is_usdc ? INCOMING_USDC_MIN : INCOMING_SPL_MIN;
        if (d > thr) {
            const char *sym = s_tokens[i].symbol[0] ? s_tokens[i].symbol : "tokens";
            s_incoming_cb(sym, d);
        }
    }
}

static void snapshot_balances(void) {
    s_prev_sol = s_sol_balance;
    memcpy(s_prev_tokens, s_tokens, sizeof(s_prev_tokens));
    s_prev_tokens_n = s_tokens_n;
    s_prev_seeded   = true;
}

void wallet_refresh(void) {
    if (!s_ok) return;
    char *buf = alloc_rpc_buf();
    if (!buf) { ESP_LOGW(TAG, "rpc buf oom"); return; }
    refresh_sol_balance(buf);
    refresh_tokens(buf);
    free(buf);
    s_last_refresh_us = esp_timer_get_time();

    detect_incoming();
    snapshot_balances();

    ESP_LOGI(TAG, "refresh done — %.4f SOL, %u tokens",
             s_sol_balance, (unsigned)s_tokens_n);

    // After balances settle, top up the device's USDC float from the vault
    // if it's run low. Refill is a separate tx (vault_execute → SPL
    // TransferChecked); it can take a few hundred ms but doesn't block the
    // foreground because the wallet refresh runs on its own task.
    char refill_txid[96];
    refill_check_and_maybe_run(refill_txid, sizeof refill_txid);
}

// ---------------------------------------------------------------------------
// Background task
// ---------------------------------------------------------------------------
static void wallet_task(void *arg) {
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_trigger, portMAX_DELAY) == pdTRUE) {
            wallet_refresh();
        }
    }
}

static void ensure_task(void) {
    if (s_task) return;
    s_trigger = xSemaphoreCreateBinary();
    // 8 KB — two TLS handshakes + cJSON parse of up to 16 KB body.
    xTaskCreatePinnedToCore(wallet_task, "wallet", 8192, NULL, 5, &s_task, 1);
}

void wallet_request_refresh(void) {
    ensure_task();
    if (s_trigger) xSemaphoreGive(s_trigger);
}

// ---------------------------------------------------------------------------
// AI context
// ---------------------------------------------------------------------------
// Pretty-print a token amount with decimals that scale to magnitude.
static int fmt_amount(char *out, size_t cap, double a) {
    if (a >= 1000)     return snprintf(out, cap, "%.0f", a);
    else if (a >= 10)  return snprintf(out, cap, "%.2f", a);
    else               return snprintf(out, cap, "%.4f", a);
}

// Append up to cap-1-w chars from a snprintf result; update *w. Returns
// false if the caller should stop writing (out of room / snprintf error).
static bool ctx_append(char *out, size_t cap, size_t *w, int n) {
    if (n < 0) return false;
    size_t room = (cap == 0 || *w >= cap - 1) ? 0 : (cap - 1 - *w);
    *w += (size_t)n < room ? (size_t)n : room;
    return *w < cap - 1;
}

void wallet_context(char *out, size_t cap, double sol_usd_price) {
    if (!out || cap == 0) return;
    if (!s_ok) {
        snprintf(out, cap,
                 "(No wallet key configured — Daemon has no holdings yet.)");
        return;
    }

    size_t w = 0;
    int n;

    n = snprintf(out + w, cap - w,
                 "Your wallet address is %s (Solana mainnet).\n", s_pubkey);
    if (!ctx_append(out, cap, &w, n)) return;

    if (sol_usd_price > 0) {
        n = snprintf(out + w, cap - w, "Native SOL balance: %.4f (~$%.2f).\n",
                     s_sol_balance, s_sol_balance * sol_usd_price);
    } else {
        n = snprintf(out + w, cap - w, "Native SOL balance: %.4f.\n",
                     s_sol_balance);
    }
    if (!ctx_append(out, cap, &w, n)) return;

    if (s_tokens_n == 0) {
        snprintf(out + w, cap - w, "SPL tokens: none.\n");
        return;
    }

    n = snprintf(out + w, cap - w, "SPL token holdings: ");
    if (!ctx_append(out, cap, &w, n)) return;

    for (size_t i = 0; i < s_tokens_n; ++i) {
        if (i > 0) {
            n = snprintf(out + w, cap - w, ", ");
            if (!ctx_append(out, cap, &w, n)) return;
        }
        char num[24];
        fmt_amount(num, sizeof(num), s_tokens[i].amount);
        const char *tag = s_tokens[i].symbol[0] ? s_tokens[i].symbol
                                                : s_tokens[i].mint;
        n = snprintf(out + w, cap - w, "%s %s", num, tag);
        if (!ctx_append(out, cap, &w, n)) return;
    }
    if (w < cap - 1) snprintf(out + w, cap - w, ".\n");
}
