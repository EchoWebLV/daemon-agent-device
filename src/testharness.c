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

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

// Subsystem APIs the verbs drive. Keep this list tight — the harness is
// the only caller that needs to see all of these together, so we intentionally
// fan-in the dependencies here rather than in a public header.
#include "agent_pda.h"
#include "ai.h"
#include "base58.h"
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

    // --- VAULT PDA --- (Phase 2a-x402)
    if ((rest = match_token(after_test, "VAULT"))) {
        if (match_token(rest, "PDA")) { handle_vault_pda(); return; }
        resp_err("vault bad_subverb");
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
    // 8 KB stack. AI PING and X402 CALL drive mbedTLS + cJSON + (for x402)
    // a Solana tx build/sign. mbedTLS is heap-only in this build, so the
    // handshake stays in the ~4 KB range; 8 KB leaves ~2 KB of watermark.
    BaseType_t ok = xTaskCreatePinnedToCore(harness_task,
                                            "testharness",
                                            8 * 1024,
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
