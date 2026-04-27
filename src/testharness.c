// ---------------------------------------------------------------------------
//  Host-driven E2E test harness. Protocol + case list:
//      docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md
//
//  Task-based design: one dedicated FreeRTOS task blocks on fgets(stdin) and
//  dispatches each line. BEGIN sets s_in_test_mode; the flag itself isn't
//  required for correctness (verbs work whenever they're called) but it
//  mirrors the spec's lifecycle and gives external modules something to
//  consult if they ever want to pause background work during a run.
//
//  Every response line starts with "TEST OK " or "TEST ERR " and ends with
//  a single \n. Any line NOT starting with "TEST " is ignored — the host
//  filters the same way. That lets regular ESP_LOGx output flow through
//  the same USB-CDC link without confusing the parser on either side.
// ---------------------------------------------------------------------------
#include "testharness.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "secrets.h"

// Subsystem APIs the verbs drive. Keep this list tight — the harness is
// the only caller that needs to see all of these together, so we intentionally
// fan-in the dependencies here rather than in a public header.
#include "agent_pda.h"
#include "ai.h"
#include "base58.h"
#include "pin.h"
#include "refill.h"
#include "solana_tx.h"
#include "swap.h"
#include "ui.h"
#include "wallet.h"
#include "wifi_sta.h"
#include "x402.h"

static const char *TAG = "testharness";

// ---------- state ----------------------------------------------------------

// Single static flag flipped by BEGIN/END. Read atomically via test_harness_
// in_test_mode(); writes happen only from the harness task.
static volatile bool s_in_test_mode = false;

// Screen transitions animate for ~220 ms (see ui.c:load_screen_anim). After
// a SCREEN FORCE or SWIPE the harness waits a little longer so any test that
// immediately re-reads ui_current_screen_name() sees the settled state, and
// so a back-to-back command doesn't collide with the tail of the animation.
#define SCREEN_ANIM_SETTLE_MS 280

// ---------- small response helpers -----------------------------------------

static void resp_ok(const char *rest) {
    // Single printf to keep the full line atomic relative to any ESP_LOGx
    // output racing for the UART — stdio is line-buffered under the console
    // VFS, so one printf call maps to one write.
    printf("TEST OK %s\n", rest);
    fflush(stdout);
}

static void resp_err(const char *rest) {
    printf("TEST ERR %s\n", rest);
    fflush(stdout);
}

// ---------- line parsing ---------------------------------------------------

// Strip trailing CR / LF / spaces in place. fgets keeps the \n and the host
// sometimes ships \r\n; we don't want "BEGIN\r" to mismatch "BEGIN".
static void rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

// The protocol is space-delimited but SSIDs can legally contain spaces,
// so we rewrite spaces to underscores on the wire and the host reverses it.
// Empty SSID becomes "-" so the column stays non-blank.
static void safe_ssid(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t j = 0;
    if (src) {
        for (size_t i = 0; src[i] && j + 1 < cap; i++) {
            dst[j++] = (src[i] == ' ') ? '_' : src[i];
        }
    }
    dst[j] = '\0';
    if (!dst[0] && cap > 1) { dst[0] = '-'; dst[1] = '\0'; }
}

// Case-insensitive match of the first token. Returns pointer to rest-of-line
// (after the matched token and any intermediate whitespace), or NULL if no
// match. Empty `token` is a programming error and asserts via NULL return.
static const char *match_token(const char *line, const char *token) {
    if (!line || !token || !*token) return NULL;
    size_t tl = strlen(token);
    for (size_t i = 0; i < tl; i++) {
        if (toupper((unsigned char)line[i]) != toupper((unsigned char)token[i])) {
            return NULL;
        }
    }
    // Must be followed by whitespace or end-of-line so "BEGIN" doesn't match
    // a longer word like "BEGINNER".
    char after = line[tl];
    if (after != '\0' && after != ' ' && after != '\t') return NULL;
    const char *p = line + tl;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// ---------- verb implementations (MVP) -------------------------------------

static void handle_begin(void) {
    s_in_test_mode = true;
    resp_ok("begin");
}

static void handle_end(void) {
    s_in_test_mode = false;
    resp_ok("end");
}

static void handle_ping(void) {
    // Uptime in ms — esp_timer_get_time returns microseconds.
    uint64_t ms = (uint64_t)(esp_timer_get_time() / 1000);
    char buf[48];
    snprintf(buf, sizeof(buf), "ping %" PRIu64, ms);
    resp_ok(buf);
}

static void handle_heap(void) {
    size_t internal = esp_get_free_heap_size();
    size_t psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char buf[64];
    snprintf(buf, sizeof(buf), "heap %u %u",
             (unsigned)internal, (unsigned)psram);
    resp_ok(buf);
}

static void handle_version(void) {
    // IDF version is a compile-time string macro. __DATE__ / __TIME__ come
    // from preprocessor and reflect when this file was last compiled, which
    // is what the host wants ("did we actually flash the new build?").
    char buf[96];
    snprintf(buf, sizeof(buf), "version %s %s %s",
             IDF_VER, __DATE__, __TIME__);
    resp_ok(buf);
}

// ---------- verb implementations (Phase 2) ---------------------------------
//
// Each verb maps to one ui/wallet/wifi/ai/x402 public API. The harness does
// no assertion itself — it just surfaces the raw subsystem state so the host
// can decide what counts as a PASS. Response strings follow the protocol
// in docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md.

static void handle_screen_get(void) {
    char buf[48];
    snprintf(buf, sizeof(buf), "screen %s", ui_current_screen_name());
    resp_ok(buf);
}

static void handle_screen_force(const char *name) {
    // Trim any argument that the host might have sent with trailing slop
    // (e.g. a stray space). match_token already consumed the leading
    // whitespace, so `name` points directly at the screen name.
    if      (!strcasecmp(name, "creature")) ui_show_creature();
    else if (!strcasecmp(name, "wallet"))   ui_show_wallet();
    else if (!strcasecmp(name, "settings")) ui_show_settings();
    else if (!strcasecmp(name, "wifi"))     ui_show_wifi();
    else {
        char msg[64];
        snprintf(msg, sizeof(msg), "screen unknown %.32s", name);
        resp_err(msg);
        return;
    }
    // Let the slide animation finish before we echo back / let the next
    // command in. lv_screen_active() flips to the new screen immediately, so
    // the ui_current_screen_name() readback is already correct — this delay
    // mostly prevents the next TEST SWIPE / FORCE from riding into the tail
    // of the current transition.
    vTaskDelay(pdMS_TO_TICKS(SCREEN_ANIM_SETTLE_MS));
    // Echo the active screen so the host sees a confirmed, not a hoped-for,
    // transition. We deliberately re-query through ui_current_screen_name()
    // instead of echoing `name` — catches a bug where the loader silently
    // declined the switch.
    char buf[48];
    snprintf(buf, sizeof(buf), "screen %s", ui_current_screen_name());
    resp_ok(buf);
}

static void handle_screen_paint(void) {
    // Time one full repaint round. ui_force_repaint() runs lv_refr_now()
    // inside the port lock, so this call is synchronous from the harness
    // task's perspective — the elapsed wall-clock is the paint cost.
    uint64_t t0 = esp_timer_get_time();
    ui_force_repaint();
    uint32_t dt_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    char buf[48];
    snprintf(buf, sizeof(buf), "paint %" PRIu32, dt_ms);
    resp_ok(buf);
}

static void handle_tap(const char *args) {
    // TAP on this build is a protocol-level acknowledgement rather than a
    // real synthetic touch: LVGL owns the CST328 input device and the
    // screens that exist on ESP-IDF today (creature / wallet / settings /
    // wifi) are reached through gestures or explicit screen forces rather
    // than tile taps. The host runner sends TAP anyway so the protocol
    // stays compatible with the spec; we parse the coords as a sanity
    // check and report OK. If a future screen needs real tap injection,
    // replace the body with an lv_indev point-and-click sequence.
    int x = 0, y = 0;
    if (sscanf(args, "%d %d", &x, &y) != 2) {
        resp_err("tap bad_args");
        return;
    }
    (void)x; (void)y;
    resp_ok("tap");
}

static void handle_swipe(const char *args) {
    // Spec encoding: LEFT / RIGHT / UP / DOWN (case-insensitive).
    int dir = -1;
    if      (!strcasecmp(args, "LEFT"))  dir = 0;
    else if (!strcasecmp(args, "RIGHT")) dir = 1;
    else if (!strcasecmp(args, "UP"))    dir = 2;
    else if (!strcasecmp(args, "DOWN"))  dir = 3;
    else {
        char msg[48];
        snprintf(msg, sizeof(msg), "swipe bad_dir %.16s", args);
        resp_err(msg);
        return;
    }
    ui_inject_swipe(dir);
    // ui_inject_swipe calls through to ui_show_* which kicks a 220 ms slide
    // animation. Same rationale as handle_screen_force for waiting it out.
    vTaskDelay(pdMS_TO_TICKS(SCREEN_ANIM_SETTLE_MS));
    resp_ok("swipe");
}

static void handle_wifi_status(void) {
    if (wifi_sta_is_connected()) {
        char ssid[33];
        safe_ssid(ssid, sizeof(ssid), wifi_sta_current_ssid());
        char buf[64];
        snprintf(buf, sizeof(buf), "wifi connected %s %d",
                 ssid, (int)wifi_sta_current_rssi());
        resp_ok(buf);
    } else {
        resp_ok("wifi disconnected - 0");
    }
}

static void handle_wifi_scan(void) {
    // Bounded output buffer; 16 APs is plenty for any normal environment
    // and matches the order of magnitude a phone's Wi-Fi picker shows.
    enum { SCAN_CAP = 16 };
    wifi_sta_scan_ap_t aps[SCAN_CAP];
    size_t n = wifi_sta_scan(aps, SCAN_CAP);
    char buf[48];
    snprintf(buf, sizeof(buf), "wifi scan %u", (unsigned)n);
    resp_ok(buf);
    // Per-AP continuation lines. Format matches the spec:
    //   TEST NET <ssid> <rssi> <enc>
    for (size_t i = 0; i < n; i++) {
        char ssid[33];
        safe_ssid(ssid, sizeof(ssid), aps[i].ssid);
        printf("TEST NET %s %d %s\n",
               ssid, (int)aps[i].rssi,
               aps[i].auth_open ? "OPEN" : "WPA");
        fflush(stdout);
    }
}

static void handle_wallet_pubkey(void) {
    const char *pk = wallet_pubkey();
    if (!pk || !pk[0]) {
        resp_err("wallet no_pubkey");
        return;
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "pubkey %s", pk);
    resp_ok(buf);
}

// Derives agent_root + vault PDAs from the OWNER pubkey (recovery_authority,
// configured in secrets.h as OWNER_PUBKEY) and prints both in base58.
// Cross-check against `setup-test-vault.ts` output for the same owner —
// they must match exactly.
static void handle_vault_pda(void) {
    const uint8_t *owner = wallet_owner_pubkey_bytes();
    if (!owner) { resp_err("vault no_owner_pubkey"); return; }

    uint8_t root[32], vault[32];
    int rb = agent_pda_derive_root(owner, root);
    if (rb < 0) { resp_err("vault root_derive_failed"); return; }
    int vb = agent_pda_derive_vault(root, vault);
    if (vb < 0) { resp_err("vault vault_derive_failed"); return; }

    char rs[64], vs[64], buf[200];
    base58_encode(root,  32, rs, sizeof rs);
    base58_encode(vault, 32, vs, sizeof vs);
    snprintf(buf, sizeof buf, "agent_root %s vault %s root_bump %d vault_bump %d",
             rs, vs, rb, vb);
    resp_ok(buf);
}

// Hardcoded vault state — mirrors target/test-vault-mainnet.json. Re-run
// `yarn ts-node scripts/setup-test-vault.ts mainnet <DEVICE_PUBKEY>` if
// any of these ever drift (they shouldn't for mainnet — real USDC mint
// is canonical, ATAs are deterministic from owner+mint).
static const char *TV_USDC_MINT      = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
static const char *TV_VAULT_USDC_ATA = "Hb1L7xMqwzvdNVbSPMx8zkAwwkHzzZqLnkvzopyrXtj1";
static const char *TV_OWNER_USDC_ATA = "CDbc47nCDiVfr7LBS2Xk8cyyrbsobZv1USvSdmmHrpsQ";

// Direct-RPC sendTransaction. Uses the daemon's normal Helius mainnet URL
// (wallet_rpc_url) — the program is on mainnet and so is the vault. Surfaces
// preflight logs to ESP_LOGW for debugging.
static bool rpc_send_test_tx(const char *signed_tx_b64,
                             char *txid_out, size_t cap)
{
    size_t req_cap = strlen(signed_tx_b64) + 256;
    char  *req     = malloc(req_cap);
    if (!req) return false;
    snprintf(req, req_cap,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sendTransaction\","
        "\"params\":[\"%s\","
        "{\"encoding\":\"base64\",\"skipPreflight\":false,"
        "\"preflightCommitment\":\"processed\"}]}",
        signed_tx_b64);

    char *rsp = malloc(4096);  // heap, not stack — keeps harness stack lean
    if (!rsp) { free(req); return false; }
    bool ok = rpc_call(req, rsp, 4096);
    free(req);
    if (!ok) { free(rsp); return false; }

    cJSON *root = cJSON_Parse(rsp);
    free(rsp);
    if (!root) return false;
    bool got = false;
    const cJSON *result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsString(result) && result->valuestring && result->valuestring[0]) {
        strlcpy(txid_out, result->valuestring, cap);
        got = true;
    } else {
        const cJSON *err = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsObject(err)) {
            const cJSON *msg = cJSON_GetObjectItem(err, "message");
            if (cJSON_IsString(msg)) ESP_LOGW(TAG, "rpc err: %s", msg->valuestring);
            const cJSON *data = cJSON_GetObjectItem(err, "data");
            const cJSON *logs = data ? cJSON_GetObjectItem(data, "logs") : NULL;
            if (cJSON_IsArray(logs)) {
                int n = cJSON_GetArraySize(logs);
                for (int i = 0; i < n; i++) {
                    const cJSON *ln = cJSON_GetArrayItem(logs, i);
                    if (cJSON_IsString(ln)) ESP_LOGW(TAG, "  log: %s", ln->valuestring);
                }
            }
        }
    }
    cJSON_Delete(root);
    return got;
}

// Builds vault_execute(TransferChecked from vault → owner, amount tokens, 6 dec)
// for the test mint and submits it. Returns the resulting tx signature on
// success — verify on solscan.io/?cluster=devnet.
static void handle_vault_transfer(const char *args) {
    // Argv: <amount-tokens-with-6-dec> e.g. "10000" = 0.01 USDC
    uint64_t amount = strtoull(args, NULL, 10);
    if (amount == 0) { resp_err("vault transfer bad_amount"); return; }

    const uint8_t *device_pk = wallet_pubkey_bytes();
    const uint8_t *owner_pk  = wallet_owner_pubkey_bytes();
    if (!device_pk || !owner_pk) { resp_err("vault no_keys"); return; }

    uint8_t agent_root[32], vault_pk[32];
    if (agent_pda_derive_root(owner_pk, agent_root) < 0)        { resp_err("vault root_derive"); return; }
    if (agent_pda_derive_vault(agent_root, vault_pk) < 0)       { resp_err("vault vault_derive"); return; }

    uint8_t mint_pk[32], vault_ata[32], owner_ata[32];
    if (base58_decode(TV_USDC_MINT,      mint_pk,    32) != 32) { resp_err("decode mint"); return; }
    if (base58_decode(TV_VAULT_USDC_ATA, vault_ata,  32) != 32) { resp_err("decode vault_ata"); return; }
    if (base58_decode(TV_OWNER_USDC_ATA, owner_ata,  32) != 32) { resp_err("decode owner_ata"); return; }

    uint8_t blockhash[32];
    if (!fetch_recent_blockhash(blockhash)) { resp_err("vault blockhash"); return; }

    // Inner: SPL TransferChecked(vault_ata → owner_ata, amount, 6 dec).
    uint8_t inner_data[10];
    inner_data[0] = 0x0C;
    for (int i = 0; i < 8; i++) inner_data[1 + i] = (uint8_t)(amount >> (i * 8));
    inner_data[9] = 6;

    agent_meta_t inner_metas[] = {
        { vault_ata,  false, true  },   // source
        { mint_pk,    false, false },   // mint
        { owner_ata,  false, true  },   // destination
        { vault_pk,   true,  false },   // authority (vault PDA — signs via CPI)
    };

    uint8_t outer_ix_data[256];
    agent_meta_t outer_metas[16];
    size_t outer_meta_count = 0;
    int outer_data_len = agent_pda_build_vault_execute_ix(
        vault_pk, device_pk, SPL_TOKEN_PROGRAM_ID,
        inner_metas, sizeof inner_metas / sizeof inner_metas[0],
        inner_data, sizeof inner_data,
        outer_ix_data, sizeof outer_ix_data,
        outer_metas, &outer_meta_count, sizeof outer_metas / sizeof outer_metas[0]);
    if (outer_data_len < 0) { resp_err("vault build_ix"); return; }

    // Translate agent_meta_t → solana_ix_account_t for the v2 builder.
    solana_ix_account_t v2_accs[16];
    for (size_t i = 0; i < outer_meta_count; i++) {
        v2_accs[i].pubkey      = outer_metas[i].pubkey;
        v2_accs[i].is_signer   = outer_metas[i].is_signer;
        v2_accs[i].is_writable = outer_metas[i].is_writable;
    }
    solana_ix_v2_t ix = {
        .program_id    = AGENT_PROGRAM_ID,
        .accounts      = v2_accs,
        .account_count = outer_meta_count,
        .data          = outer_ix_data,
        .data_len      = (size_t)outer_data_len,
    };
    // Test path: device pays its own fees (no x402 facilitator), so fee_payer
    // = device_pk too. That makes num_required_sigs = 1 and slot 0 carries
    // our signature.
    solana_tx_input_v2_t txin = {
        .fee_payer      = device_pk,
        .signer_pubkey  = device_pk,
        .blockhash      = blockhash,
        .ixs            = &ix,
        .ix_count       = 1,
        .cu_limit       = 50000,
        .cu_price_micro = 1,
    };
    char tx_b64[1536];
    int tx_len = solana_build_tx_v2_base64(&txin, tx_b64, sizeof tx_b64);
    if (tx_len <= 0) { resp_err("vault build_tx"); return; }

    char txid[96];
    if (!rpc_send_test_tx(tx_b64, txid, sizeof txid)) { resp_err("vault rpc_send"); return; }

    char buf[160];
    snprintf(buf, sizeof buf, "txid %s", txid);
    resp_ok(buf);
}

// Phase 2b PIN verbs: drive the seal/unlock/wipe primitives from the host
// before the LVGL keypad UI is wired in.
static void handle_pin_status(void) {
    bool set = pin_is_set();
    int  rem = pin_attempts_remaining();
    char buf[64];
    snprintf(buf, sizeof buf, "set=%d remaining=%d", set ? 1 : 0, rem);
    resp_ok(buf);
}

// TEST PIN SETUP <pin>  → seals the secrets.h SOLANA_KEY under <pin>.
// Same bytes wallet_begin already loads — sealing wires the PIN gate ON TOP
// of the existing key without changing the on-chain identity. After this
// lands, pin_unlock(<pin>) restores those exact bytes.
static void handle_pin_setup(const char *args) {
    char pin[PIN_MAX_LEN + 1] = {0};
    if (!args || !args[0]) { resp_err("pin setup usage <pin>"); return; }
    strlcpy(pin, args, sizeof pin);
    for (size_t i = strlen(pin); i > 0 && (pin[i-1] == ' ' || pin[i-1] == '\t'); ) {
        pin[--i] = '\0';
    }

    const char *seed_b58 = SOLANA_KEY;
    if (!seed_b58 || !seed_b58[0] || strncmp(seed_b58, "PASTE-", 6) == 0) {
        resp_err("pin no_seed"); return;
    }

    uint8_t seed_buf[PIN_MAX_SEED_LEN];
    int n = base58_decode(seed_b58, seed_buf, sizeof seed_buf);
    if (n != 32 && n != 64) { resp_err("pin seed_decode"); return; }

    pin_status_t st = pin_setup(pin, seed_buf, (size_t)n);
    memset(seed_buf, 0, sizeof seed_buf);
    if (st != PIN_OK) {
        char m[32]; snprintf(m, sizeof m, "pin setup err=%d", st);
        resp_err(m); return;
    }
    resp_ok("sealed");
}

static void handle_pin_unlock(const char *args) {
    char pin[PIN_MAX_LEN + 1] = {0};
    if (!args || !args[0]) { resp_err("pin unlock usage <pin>"); return; }
    strlcpy(pin, args, sizeof pin);
    for (size_t i = strlen(pin); i > 0 && (pin[i-1] == ' ' || pin[i-1] == '\t'); ) {
        pin[--i] = '\0';
    }

    uint8_t seed[PIN_MAX_SEED_LEN]; size_t seed_len = 0;
    pin_status_t st = pin_unlock(pin, seed, sizeof seed, &seed_len);
    memset(seed, 0, sizeof seed);                // never log seed bytes
    if (st == PIN_OK) {
        char buf[64];
        snprintf(buf, sizeof buf, "unlocked seed_len=%zu remaining=%d",
                 seed_len, pin_attempts_remaining());
        resp_ok(buf);
    } else if (st == PIN_ERR_BAD_PIN) {
        char buf[64];
        snprintf(buf, sizeof buf, "wrong remaining=%d", pin_attempts_remaining());
        resp_err(buf);
    } else if (st == PIN_ERR_WIPED) {
        resp_err("pin wiped");
    } else if (st == PIN_ERR_NOT_SET) {
        resp_err("pin not_set");
    } else {
        char buf[32]; snprintf(buf, sizeof buf, "pin err=%d", st);
        resp_err(buf);
    }
}

static void handle_pin_wipe(void) {
    pin_status_t st = pin_wipe();
    if (st == PIN_OK) resp_ok("wiped");
    else { char b[32]; snprintf(b, sizeof b, "pin err=%d", st); resp_err(b); }
}

// Force a refill from vault → device USDC ATA. Argv: micro-USDC amount.
// Returns the resulting tx signature on success.
static void handle_vault_refill(const char *args) {
    uint64_t amount = strtoull(args, NULL, 10);
    if (amount == 0) { resp_err("refill bad_amount"); return; }
    char txid[96];
    if (!refill_run_amount(amount, txid, sizeof txid)) {
        resp_err("refill failed");
        return;
    }
    char buf[128];
    snprintf(buf, sizeof buf, "txid %s", txid);
    resp_ok(buf);
}

// Encodes an empty vault_execute payload to expose the Anchor discriminator
// in hex. Must match sha256("global:vault_execute")[0..8] = b2c50da89714ae28.
static void handle_vault_disc(void) {
    uint8_t buf[256];
    agent_meta_t metas[4];
    size_t mc = 0;
    uint8_t zeroes[32] = {0};
    int n = agent_pda_build_vault_execute_ix(
        zeroes, zeroes, zeroes,
        NULL, 0, NULL, 0,
        buf, sizeof buf, metas, &mc, sizeof metas / sizeof metas[0]);
    if (n < 8) { resp_err("disc encode_failed"); return; }
    char hex[32];
    snprintf(hex, sizeof hex,
             "%02x%02x%02x%02x%02x%02x%02x%02x",
             buf[0], buf[1], buf[2], buf[3],
             buf[4], buf[5], buf[6], buf[7]);
    char out[64];
    snprintf(out, sizeof out, "disc %s", hex);
    resp_ok(out);
}

static void handle_wallet_balance(void) {
    // Force a fresh two-call RPC round-trip so the host isn't reading a
    // minute-old cache. wallet_refresh() blocks; that's fine — the harness
    // task has no latency SLA and the host budgets 5 s for this verb.
    wallet_refresh();
    char buf[80];
    snprintf(buf, sizeof(buf), "balance %.9f %.6f",
             wallet_sol_balance(), wallet_usdc_amount());
    resp_ok(buf);
}

static void handle_ai_ping(void) {
    // One-shot prompt, minimal text so the round-trip stays cheap. We don't
    // care what the reply says, only whether ai_ask_one_shot() succeeded
    // (meaning x402 paid, the LLM returned, and we parsed a non-empty
    // assistant message). Latency is the total wall-clock including x402
    // sign+submit, so the host's 8 s budget is generous.
    uint64_t t0 = esp_timer_get_time();
    char reply[256];
    bool ok = ai_ask_one_shot("ping", reply, sizeof(reply));
    uint32_t dt_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (!ok) {
        char msg[96];
        snprintf(msg, sizeof(msg), "ai no_response dt=%" PRIu32, dt_ms);
        resp_err(msg);
        return;
    }
    // 200 is a sentinel; ai_ask_one_shot doesn't bubble up a concrete
    // status. If the call returned true, x402 got a 200 by definition.
    char buf[48];
    snprintf(buf, sizeof(buf), "ai 200 %" PRIu32, dt_ms);
    resp_ok(buf);
}

static void handle_x402_call(const char *url) {
    // The host sends one URL per call. For the /chat/completions endpoint
    // the server actually wants a chat body; for anything else we POST a
    // bare {} so the facilitator sees a valid JSON request. This avoids
    // a parallel GET verb while keeping the harness URL-agnostic.
    const char *body;
    if (strstr(url, "/chat/completions")) {
        body = "{\"model\":\"claude-haiku-4.5\","
               "\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],"
               "\"max_tokens\":16}";
    } else {
        body = "{}";
    }

    // 4 KB response scratch from PSRAM — internal RAM is precious during an
    // in-flight x402 round-trip (TLS handshake + Solana tx build). Facilitator
    // replies are a few hundred bytes; anything bigger truncates.
    #define X402_SCRATCH 4096
    char *scratch = (char *)heap_caps_malloc(X402_SCRATCH, MALLOC_CAP_SPIRAM);
    if (!scratch) {
        resp_err("x402 scratch_oom");
        return;
    }
    scratch[0] = '\0';

    uint64_t t0 = esp_timer_get_time();
    x402_result_t r = (x402_result_t){0};
    x402_post(url, body, NULL, scratch, X402_SCRATCH, &r);
    uint32_t dt_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    free(scratch);

    if (r.status == 0) {
        char msg[128];
        // r.error is short and bounded (x402_result_t::error is 96 bytes),
        // so no truncation danger. dt exposes where the failure landed on
        // the timeline (fast = signing error; slow = network stall).
        snprintf(msg, sizeof(msg), "x402 %s dt=%" PRIu32,
                 r.error[0] ? r.error : "unknown", dt_ms);
        resp_err(msg);
        return;
    }

    // paid_usdc_base = USDC in 6-decimal base units. Round-half-to-even via
    // llround + explicit cast; using %.0f on cost_usd * 1e6 loses pennies
    // at high balances because doubles can't exactly represent all µUSDC
    // values.
    uint64_t paid_base = (uint64_t)llround(r.cost_usd * 1000000.0);
    char buf[96];
    snprintf(buf, sizeof(buf), "x402 %d %" PRIu64 " %" PRIu32,
             r.status, paid_base, dt_ms);
    resp_ok(buf);
}

// ---------- dispatcher -----------------------------------------------------

static void dispatch_line(const char *line) {
    // All of our verbs start with "TEST ". Anything else is someone else's
    // output and we silently ignore it — keeps regular logs from triggering
    // "unknown verb" noise.
    const char *after_test = match_token(line, "TEST");
    if (!after_test) return;

    const char *rest;

    // --- Lifecycle + liveness ----------------------------------------------
    if (match_token(after_test, "BEGIN"))    { handle_begin();   return; }
    if (match_token(after_test, "END"))      { handle_end();     return; }
    if (match_token(after_test, "PING"))     { handle_ping();    return; }
    if (match_token(after_test, "HEAP"))     { handle_heap();    return; }
    if (match_token(after_test, "VERSION"))  { handle_version(); return; }

    // --- SCREEN GET / FORCE <name> / PAINT ----------------------------------
    if ((rest = match_token(after_test, "SCREEN"))) {
        if (match_token(rest, "GET"))    { handle_screen_get();   return; }
        if (match_token(rest, "PAINT"))  { handle_screen_paint(); return; }
        const char *args;
        if ((args = match_token(rest, "FORCE"))) { handle_screen_force(args); return; }
        resp_err("screen bad_subverb");
        return;
    }

    // --- Touch / gesture injection -----------------------------------------
    if ((rest = match_token(after_test, "TAP")))   { handle_tap(rest);   return; }
    if ((rest = match_token(after_test, "SWIPE"))) { handle_swipe(rest); return; }

    // --- WIFI STATUS / SCAN -------------------------------------------------
    if ((rest = match_token(after_test, "WIFI"))) {
        if (match_token(rest, "STATUS")) { handle_wifi_status(); return; }
        if (match_token(rest, "SCAN"))   { handle_wifi_scan();   return; }
        resp_err("wifi bad_subverb");
        return;
    }

    // --- WALLET PUBKEY / BALANCE -------------------------------------------
    if ((rest = match_token(after_test, "WALLET"))) {
        if (match_token(rest, "PUBKEY"))  { handle_wallet_pubkey();  return; }
        if (match_token(rest, "BALANCE")) { handle_wallet_balance(); return; }
        resp_err("wallet bad_subverb");
        return;
    }

    // --- VAULT PDA / DISC / TRANSFER <amount> / REFILL <amount> --- (Phase 2a)
    if ((rest = match_token(after_test, "VAULT"))) {
        const char *args;
        if (match_token(rest, "PDA"))  { handle_vault_pda();  return; }
        if (match_token(rest, "DISC")) { handle_vault_disc(); return; }
        if ((args = match_token(rest, "TRANSFER"))) { handle_vault_transfer(args); return; }
        if ((args = match_token(rest, "REFILL")))   { handle_vault_refill(args);   return; }
        resp_err("vault bad_subverb");
        return;
    }

    // --- PIN STATUS / SETUP <pin> / UNLOCK <pin> / WIPE --- (Phase 2b)
    if ((rest = match_token(after_test, "PIN"))) {
        const char *args;
        if (match_token(rest, "STATUS")) { handle_pin_status(); return; }
        if (match_token(rest, "WIPE"))   { handle_pin_wipe();   return; }
        if ((args = match_token(rest, "SETUP")))  { handle_pin_setup(args);  return; }
        if ((args = match_token(rest, "UNLOCK"))) { handle_pin_unlock(args); return; }
        resp_err("pin bad_subverb");
        return;
    }

    // --- AI PING ------------------------------------------------------------
    if ((rest = match_token(after_test, "AI"))) {
        if (match_token(rest, "PING")) { handle_ai_ping(); return; }
        resp_err("ai bad_subverb");
        return;
    }

    // --- X402 CALL <url> ----------------------------------------------------
    if ((rest = match_token(after_test, "X402"))) {
        const char *args;
        if ((args = match_token(rest, "CALL"))) { handle_x402_call(args); return; }
        resp_err("x402 bad_subverb");
        return;
    }

    // --- SWAP RESOLVE <sym> | SWAP DEMO | SWAP SIG <b64> | SWAP DRY_RUN ---
    if ((rest = match_token(after_test, "SWAP"))) {
        // RESOLVE <sym>  →  TEST OK <mint>:<decimals> | TEST ERR unresolved
        const char *args;
        if ((args = match_token(rest, "RESOLVE"))) {
            char buf[64];
            if (swap_resolve_for_test(args, buf, sizeof(buf))) {
                resp_ok(buf);
            } else {
                resp_err("unresolved");
            }
            return;
        }
        // DEMO  →  TEST OK confirm | cancel_release | cancel_swipe | cancel_timeout
        if (match_token(rest, "DEMO")) {
            resp_ok(swap_show_demo_for_test());
            return;
        }
        // SIG <b64-unsigned-v0-tx>  →  TEST OK signed | TEST ERR rc=<n>
        // Output buffer sized for SWAP_TX_B64_CAP (8192) — enough for any
        // Jupiter v6 single-tx swap we have observed.
        if ((args = match_token(rest, "SIG"))) {
            char *out = malloc(8192);
            if (!out) { resp_err("oom"); return; }
            int rc = swap_test_sig_for_test(args, out, 8192);
            if (rc == 0) {
                resp_ok("signed");
            } else {
                char m[32];
                snprintf(m, sizeof(m), "rc=%d", rc);
                resp_err(m);
            }
            free(out);
            return;
        }
        // DRY_RUN <from> <to> <amount> [slippage_bps]
        //   → TEST OK <json blob> | TEST ERR <json blob>
        if ((args = match_token(rest, "DRY_RUN"))) {
            char from[12] = {0}, to[12] = {0};
            double amount = 0;
            unsigned slip = 0;
            int parsed = sscanf(args, "%11s %11s %lf %u",
                                from, to, &amount, &slip);
            if (parsed < 3) { resp_err("usage SWAP DRY_RUN <from> <to> <amount> [slippage_bps]"); return; }
            char buf[256];
            if (swap_dry_run_for_test(from, to, amount, (uint16_t)slip, buf, sizeof(buf))) {
                resp_ok(buf);
            } else {
                resp_err(buf);  // already JSON-shaped; harness OK with raw payload
            }
            return;
        }
        resp_err("swap bad_subverb");
        return;
    }

    // Echo enough of the bad line to be useful but cap it so a giant paste
    // can't blow the output buffer.
    char msg[64];
    snprintf(msg, sizeof(msg), "unknown %.48s", after_test);
    resp_err(msg);
}

// ---------- harness task ---------------------------------------------------

// Reads one line at a time from stdin and dispatches it. Blocks on I/O —
// that's fine because this task is dedicated. Line buffer is sized to fit
// the longest verb we expect (a TEST X402 CALL <url> line, ~200 bytes).
static void harness_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "test harness task online (core=%d, prio=%d)",
             xPortGetCoreID(), uxTaskPriorityGet(NULL));

    // fgets + stdin works because PlatformIO's default ESP-IDF sdkconfig
    // routes stdio to the USB-CDC console. If a future build disables CDC
    // we'd need to fall back to uart_read_bytes on UART0.
    char line[256];

    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            // EOF or error — the console driver may deliver this briefly
            // during enumeration. Back off and retry rather than spin.
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        rtrim(line);
        if (line[0] == '\0') continue;
        dispatch_line(line);
    }
}

// ---------- public API -----------------------------------------------------

bool test_harness_begin(void) {
    // 16 KB stack. The Phase 2a-x402 verbs (VAULT TRANSFER especially) build
    // an entire v0 message on the stack (3KB tx buf + 1KB msg buf) and then
    // POST a 4KB sendTransaction response — combined call chain peaks around
    // 10 KB. Earlier verbs fit in 8 KB, this one doesn't.
    BaseType_t ok = xTaskCreatePinnedToCore(harness_task,
                                            "testharness",
                                            16 * 1024,
                                            NULL,
                                            4,          // priority
                                            NULL,
                                            0);         // core 0
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn harness task");
        return false;
    }
    return true;
}

bool test_harness_in_test_mode(void) {
    return s_in_test_mode;
}
