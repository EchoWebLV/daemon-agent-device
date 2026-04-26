# AI-Driven Token Swaps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `swap_tokens` capability so the LLM (or the user via chat) can swap SOL/USDC/held-SPL on-device via Jupiter, gated by a 3-second hold-to-confirm approval modal on the device touchscreen.

**Architecture:** Three new firmware pieces — `swap.c/.h` (Jupiter HTTP + RPC + tx-signing orchestration), `swap_screen.c/.h` (LVGL approval modal with hold-to-confirm), and a synthetic `swap_tokens` tool registered into `ai.c`'s OpenAI tool array. Reuses existing `wallet_sign`, `wallet_rpc_url`, `wallet_request_refresh`, the `wallet_incoming_cb` creature-reaction path, and the existing `esp_http_client` + cJSON + mbedtls/base64 stack.

**Tech Stack:** ESP-IDF, FreeRTOS, LVGL 9 (via esp_lvgl_port), cJSON, mbedtls (base64), orlp/ed25519, esp_http_client, Jupiter v6 (`https://quote-api.jup.ag`), Solana JSON-RPC.

**Spec:** [`docs/superpowers/specs/2026-04-26-ai-swap-tokens-design.md`](../specs/2026-04-26-ai-swap-tokens-design.md)

---

## File Structure

| File                    | Status   | Responsibility                                                                  |
|-------------------------|----------|---------------------------------------------------------------------------------|
| `src/swap.h`            | Create   | Public types + `swap_request()` entry point                                     |
| `src/swap.c`            | Create   | Jupiter HTTP, sig-splice, RPC submit, orchestration, concurrency flag           |
| `src/swap_screen.h`     | Create   | Approval-modal API consumed by swap.c                                           |
| `src/swap_screen.c`     | Create   | LVGL modal: 5-line layout + hold-to-confirm arc + cancellation handling         |
| `src/CMakeLists.txt`    | Modify   | Add `swap.c` + `swap_screen.c` to SRCS                                          |
| `src/ai.c`              | Modify   | Synthetic-tool registration in `attach_tools()`; branch in `execute_tool()`     |
| `src/testharness.c`     | Modify   | Add `swap_dry_run` and `swap_test_sig` verbs                                    |

`swap.c` is split internally into clearly-bounded sections (mint resolution, Jupiter HTTP, tx signing, RPC submit, orchestration). The header surface is intentionally small — one entry point and one result type — so the rest of the firmware sees the swap pipeline as opaque.

---

## Build/test commands (used throughout)

```bash
# build only
pio run -e waveshare_esp32s3_28

# build + flash + open monitor
pio run -e waveshare_esp32s3_28 -t upload && pio device monitor -e waveshare_esp32s3_28

# run a single test-harness verb from the host
python tests/run.py "swap_dry_run USDC SOL 0.1"
```

Most tasks below end with **build** as the verification gate (firmware compiles cleanly; new symbols link). Task 7 has a deterministic verb-driven test for the tx-signing splice. The end-to-end smoke (Task 12) requires real wallet funds + network.

---

## Task 1: Module skeleton

**Files:**
- Create: `src/swap.h`
- Create: `src/swap.c`
- Create: `src/swap_screen.h`
- Create: `src/swap_screen.c`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1.1: Write `src/swap.h`**

```c
// ---------------------------------------------------------------------------
//  On-device Solana token swaps via Jupiter v6.
//
//  Single in-flight swap at a time, gated by a hardware hold-to-confirm
//  approval modal. The LLM calls this through the synthetic `swap_tokens`
//  tool registered in ai.c; user-typed swap requests reach here via the
//  same path because the LLM parses the intent.
//
//  swap_request() is synchronous from the caller's POV: it blocks the
//  calling task on an internal semaphore until the approval window
//  resolves AND the resulting on-chain swap is confirmed (or fails).
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SWAP_OK,
    SWAP_ERR_QUOTE,           // Jupiter /quote unreachable or no route
    SWAP_ERR_INSUFFICIENT,    // wallet doesn't hold enough of `from_sym`
    SWAP_ERR_CANCELLED,       // user released early, swiped, or 30s timeout
    SWAP_ERR_IN_PROGRESS,     // a swap is already running
    SWAP_ERR_BUILD,           // Jupiter /swap failed or response unparseable
    SWAP_ERR_SUBMIT,          // RPC sendTransaction rejected the tx
    SWAP_ERR_UNCONFIRMED,     // tx didn't land within 30s (txid still valid)
} swap_status_t;

typedef struct {
    swap_status_t status;
    char          txid[96];     // base58, "" on failure
    double        amount_in;    // UI units (already divided by 10^dec)
    double        amount_out;   // UI units, 0 on failure
    char          from_sym[12];
    char          to_sym[12];
    char          error_msg[64]; // short, LLM-readable
} swap_result_t;

// Synchronous: blocks the caller until the swap completes or fails. Safe
// to call from a non-LVGL task (the approval screen is opened via
// lv_async_call internally). Returns true if `out->status == SWAP_OK`.
//
// `slippage_bps == 0`        → use the per-pair default
// `slippage_bps` outside [10, 500] → silently clamped to the default
bool swap_request(const char *from_sym,
                  const char *to_sym,
                  double      amount_ui,
                  uint16_t    slippage_bps,
                  swap_result_t *out);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 1.2: Write `src/swap.c` skeleton**

```c
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

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "swap";

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
```

- [ ] **Step 1.3: Write `src/swap_screen.h`**

```c
// ---------------------------------------------------------------------------
//  Approval modal for on-device swaps. Opened from swap.c on the LVGL
//  thread via lv_async_call; the calling task waits on `done_sem`.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     from_sym[12];
    char     to_sym[12];
    double   amount_in;        // UI units
    double   amount_out;       // expected UI units
    double   min_out;          // amount_out * (1 - slippage)
    uint16_t slippage_bps;
    double   fee_sol;          // base + priority + ATA-rent if any
} swap_screen_args_t;

typedef enum {
    SWAP_UI_CONFIRM,
    SWAP_UI_CANCEL_RELEASE,
    SWAP_UI_CANCEL_SWIPE,
    SWAP_UI_CANCEL_TIMEOUT,
} swap_ui_result_t;

// Open the modal. Caller blocks on `done_sem`; on signal, `*out_result`
// holds the resolution. The modal closes itself before signalling.
// Safe to call from any task — internally schedules via lv_async_call.
void swap_screen_open(const swap_screen_args_t *args,
                      SemaphoreHandle_t        done_sem,
                      swap_ui_result_t        *out_result);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 1.4: Write `src/swap_screen.c` skeleton**

```c
// ---------------------------------------------------------------------------
//  swap_screen.c — see swap_screen.h. LVGL implementation in Task 5.
// ---------------------------------------------------------------------------
#include "swap_screen.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "swap_screen";

void swap_screen_open(const swap_screen_args_t *args,
                      SemaphoreHandle_t        done_sem,
                      swap_ui_result_t        *out_result) {
    (void)args;
    if (out_result) *out_result = SWAP_UI_CANCEL_TIMEOUT;
    ESP_LOGW(TAG, "swap_screen_open stub — auto-cancelling");
    if (done_sem) xSemaphoreGive(done_sem);
}
```

- [ ] **Step 1.5: Wire `src/CMakeLists.txt`**

Add the two new sources to the alphabetised SRCS list:

```cmake
        "settings_screen.c"
        "solana_tx.c"
        "swap.c"
        "swap_screen.c"
        "testharness.c"
```

- [ ] **Step 1.6: Build to confirm**

```bash
pio run -e waveshare_esp32s3_28
```

Expected: clean build. New TU `swap.c` + `swap_screen.c` compile, new symbols link, no warnings other than the stub-related ones.

- [ ] **Step 1.7: Commit**

```bash
git add src/swap.h src/swap.c src/swap_screen.h src/swap_screen.c src/CMakeLists.txt
git commit -m "swap: scaffold module + approval-screen module (stubs)"
```

---

## Task 2: Symbol → mint resolution

**Files:**
- Modify: `src/swap.c` (add resolution helpers)
- Modify: `src/testharness.c` (add `swap_resolve` verb)

- [ ] **Step 2.1: Add the resolver to `swap.c`**

Insert after the `static const char *TAG` line:

```c
#include "wallet.h"

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
    if (!sym || !out) return false;
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
```

- [ ] **Step 2.2: Add a `swap_resolve` test-harness verb**

In `src/testharness.c`, add to the includes:

```c
#include "swap.h"
```

Then locate the verb-dispatch switch (look for the existing `if (strncmp(line, "SWIPE", 5) == 0)` style chain) and add this verb. We expose the resolver indirectly by giving swap.c a debug-only entry. Add to `swap.h`:

```c
// Diagnostic — used by the test harness only. Returns true iff `sym`
// resolves; on success writes "<mint>:<decimals>" into `out`.
bool swap_resolve_for_test(const char *sym, char *out, size_t cap);
```

And in `swap.c`:

```c
bool swap_resolve_for_test(const char *sym, char *out, size_t cap) {
    swap_token_t t;
    if (!resolve_token(sym, &t)) return false;
    snprintf(out, cap, "%s:%u", t.mint, (unsigned)t.decimals);
    return true;
}
```

In `testharness.c`, add the verb (place it next to other single-arg verbs):

```c
// SWAP_RESOLVE <sym>  →  TEST OK <mint>:<decimals> | TEST ERR unresolved
if (strncmp(line, "SWAP_RESOLVE ", 13) == 0) {
    const char *sym = line + 13;
    while (*sym == ' ') sym++;
    char buf[64];
    if (swap_resolve_for_test(sym, buf, sizeof(buf))) {
        resp_ok(buf);
    } else {
        resp_err("unresolved");
    }
    continue;
}
```

- [ ] **Step 2.3: Build + flash**

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 2.4: Verify on device**

```bash
python tests/run.py "SWAP_RESOLVE SOL"
# Expected: TEST OK So11111111111111111111111111111111111111112:9

python tests/run.py "SWAP_RESOLVE USDC"
# Expected: TEST OK EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v:6

python tests/run.py "SWAP_RESOLVE NOPE"
# Expected: TEST ERR unresolved
```

- [ ] **Step 2.5: Commit**

```bash
git add src/swap.h src/swap.c src/testharness.c
git commit -m "swap: symbol→mint resolution (SOL/USDC/wallet SPLs)"
```

---

## Task 3: Balance check

**Files:**
- Modify: `src/swap.c` (add `has_balance` helper)

- [ ] **Step 3.1: Add helper at module scope**

After `resolve_token`, add:

```c
// Returns true if the wallet snapshot says we hold at least `amount_ui`
// of the symbol resolved by `t`. SPL holdings are matched by mint, not
// symbol — handles the "two tokens with the same ticker" case correctly.
static bool has_balance(const swap_token_t *t, double amount_ui) {
    if (!t) return false;
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
```

- [ ] **Step 3.2: Build to confirm**

```bash
pio run -e waveshare_esp32s3_28
```

- [ ] **Step 3.3: Commit**

```bash
git add src/swap.c
git commit -m "swap: balance-check helper against wallet snapshot"
```

---

## Task 4: Jupiter `/quote` integration

**Files:**
- Modify: `src/swap.c` (add HTTP helper + quote function)

- [ ] **Step 4.1: Add HTTP scaffolding**

Insert after the existing includes in `swap.c`:

```c
#include <stdlib.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"

#include "wifi_sta.h"

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
static bool https_post_json(const char *url, const char *body,
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
```

- [ ] **Step 4.2: Add `jup_get_quote` helper**

Append:

```c
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
    uint64_t atomic = (uint64_t)(amount_ui * pow10(from->decimals) + 0.5);
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

// Tiny helper since pow10 isn't standard.
static double pow10(uint8_t n) {
    double v = 1.0;
    for (uint8_t i = 0; i < n; ++i) v *= 10.0;
    return v;
}
```

Note: forward-declare `pow10` at the top of the file (right after TAG):

```c
static double pow10(uint8_t n);
```

- [ ] **Step 4.3: Build to confirm**

```bash
pio run -e waveshare_esp32s3_28
```

- [ ] **Step 4.4: Commit**

```bash
git add src/swap.c
git commit -m "swap: Jupiter v6 /quote integration + slippage clamping"
```

---

## Task 5: Approval-modal LVGL screen

**Files:**
- Modify: `src/swap_screen.c` (full implementation)

- [ ] **Step 5.1: Replace `swap_screen.c` body with the implementation**

```c
// ---------------------------------------------------------------------------
//  swap_screen.c — see swap_screen.h.
//
//  Modal layout (240×320, pixel-art / CRT-green palette):
//
//      ┌────────────────────────┐
//      │        SWAP            │
//      │                        │
//      │    5.00 USDC           │
//      │       ↓                │
//      │    0.0431 SOL          │
//      │                        │
//      │  min  0.0429 SOL       │
//      │  slip 0.5%             │
//      │  fee  0.00001 SOL      │
//      │                        │
//      │      ╭──╮              │
//      │      │ 3│  hold        │
//      │      ╰──╯              │
//      └────────────────────────┘
//
//  Hold detection: an LVGL timer at 50 Hz reads the touch indev. The
//  fill-arc reaches 100% after 3000 ms of continuous contact; release
//  before then = CANCEL_RELEASE. Any swipe gesture the LVGL indev
//  reports = CANCEL_SWIPE. 30 s with no touch at all = CANCEL_TIMEOUT.
// ---------------------------------------------------------------------------
#include "swap_screen.h"
#include "screens_common.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "swap_screen";

#define HOLD_MS         3000
#define IDLE_TIMEOUT_MS 30000
#define POLL_PERIOD_MS  20

typedef struct {
    swap_screen_args_t args;
    SemaphoreHandle_t  done_sem;
    swap_ui_result_t  *out_result;
    lv_obj_t          *prev_screen;     // restored on close
    lv_obj_t          *scr;
    lv_obj_t          *arc;
    lv_obj_t          *arc_label;
    lv_timer_t        *poll_timer;
    uint32_t           hold_ms;          // accumulated continuous-contact ms
    uint32_t           idle_ms;          // ms since last contact
    bool               touching;
} ctx_t;

static ctx_t s_ctx;       // single in-flight modal

// Pretty-print a UI amount with up to 6 decimal places, trailing zeros stripped.
static void fmt_amount(double v, char *out, size_t cap) {
    snprintf(out, cap, "%.6f", v);
    char *dot = strchr(out, '.');
    if (!dot) return;
    char *end = out + strlen(out) - 1;
    while (end > dot && *end == '0') *end-- = '\0';
    if (end == dot) *end = '\0';
}

static void close_modal(swap_ui_result_t result) {
    if (s_ctx.poll_timer) { lv_timer_del(s_ctx.poll_timer); s_ctx.poll_timer = NULL; }
    if (s_ctx.scr)        { lv_obj_del(s_ctx.scr);          s_ctx.scr        = NULL; }
    if (s_ctx.prev_screen) {
        lv_screen_load(s_ctx.prev_screen);
        s_ctx.prev_screen = NULL;
    }
    if (s_ctx.out_result) *s_ctx.out_result = result;
    if (s_ctx.done_sem)   xSemaphoreGive(s_ctx.done_sem);
    s_ctx.done_sem = NULL;
}

static void on_swipe(lv_event_t *e) {
    (void)e;
    close_modal(SWAP_UI_CANCEL_SWIPE);
}

static void poll_tick(lv_timer_t *t) {
    (void)t;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    lv_indev_state_t state = indev ? lv_indev_get_state(indev) : LV_INDEV_STATE_RELEASED;
    bool down = (state == LV_INDEV_STATE_PRESSED);

    if (down) {
        s_ctx.hold_ms += POLL_PERIOD_MS;
        s_ctx.idle_ms  = 0;
        s_ctx.touching = true;
        int pct = (int)((s_ctx.hold_ms * 100) / HOLD_MS);
        if (pct > 100) pct = 100;
        lv_arc_set_value(s_ctx.arc, pct);
        char b[16];
        int s_left = (HOLD_MS - (int)s_ctx.hold_ms + 999) / 1000;
        if (s_left < 0) s_left = 0;
        snprintf(b, sizeof(b), "%d", s_left);
        lv_label_set_text(s_ctx.arc_label, b);
        if (s_ctx.hold_ms >= HOLD_MS) {
            close_modal(SWAP_UI_CONFIRM);
            return;
        }
    } else {
        if (s_ctx.touching && s_ctx.hold_ms > 0) {
            close_modal(SWAP_UI_CANCEL_RELEASE);
            return;
        }
        s_ctx.touching = false;
        s_ctx.idle_ms += POLL_PERIOD_MS;
        if (s_ctx.idle_ms >= IDLE_TIMEOUT_MS) {
            close_modal(SWAP_UI_CANCEL_TIMEOUT);
            return;
        }
    }
}

static void build_ui(void) {
    s_ctx.scr = lv_obj_create(NULL);
    scr_apply_bg(s_ctx.scr);

    lv_obj_t *title = lv_label_create(s_ctx.scr);
    lv_label_set_text(title, "SWAP");
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    char buf[64], amt[24];
    fmt_amount(s_ctx.args.amount_in, amt, sizeof(amt));
    snprintf(buf, sizeof(buf), "%s %s", amt, s_ctx.args.from_sym);
    lv_obj_t *l_in = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_in, buf);
    lv_obj_align(l_in, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *arrow = lv_label_create(s_ctx.scr);
    lv_label_set_text(arrow, LV_SYMBOL_DOWN);
    lv_obj_align(arrow, LV_ALIGN_TOP_MID, 0, 76);

    fmt_amount(s_ctx.args.amount_out, amt, sizeof(amt));
    snprintf(buf, sizeof(buf), "%s %s", amt, s_ctx.args.to_sym);
    lv_obj_t *l_out = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_out, buf);
    lv_obj_align(l_out, LV_ALIGN_TOP_MID, 0, 100);

    fmt_amount(s_ctx.args.min_out, amt, sizeof(amt));
    snprintf(buf, sizeof(buf), "min  %s %s", amt, s_ctx.args.to_sym);
    lv_obj_t *l_min = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_min, buf);
    lv_obj_set_style_text_color(l_min, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(l_min, LV_ALIGN_TOP_LEFT, 24, 140);

    snprintf(buf, sizeof(buf), "slip %u.%02u%%",
             s_ctx.args.slippage_bps / 100, s_ctx.args.slippage_bps % 100);
    lv_obj_t *l_slip = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_slip, buf);
    lv_obj_set_style_text_color(l_slip, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(l_slip, LV_ALIGN_TOP_LEFT, 24, 162);

    fmt_amount(s_ctx.args.fee_sol, amt, sizeof(amt));
    snprintf(buf, sizeof(buf), "fee  %s SOL", amt);
    lv_obj_t *l_fee = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_fee, buf);
    lv_obj_set_style_text_color(l_fee, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(l_fee, LV_ALIGN_TOP_LEFT, 24, 184);

    s_ctx.arc = lv_arc_create(s_ctx.scr);
    lv_obj_set_size(s_ctx.arc, 80, 80);
    lv_arc_set_range(s_ctx.arc, 0, 100);
    lv_arc_set_value(s_ctx.arc, 0);
    lv_arc_set_rotation(s_ctx.arc, 270);
    lv_arc_set_bg_angles(s_ctx.arc, 0, 360);
    lv_obj_remove_style(s_ctx.arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ctx.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_ctx.arc, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ctx.arc, SCR_COLOR_ACCENT_HI, LV_PART_INDICATOR);
    lv_obj_align(s_ctx.arc, LV_ALIGN_BOTTOM_MID, 0, -36);

    s_ctx.arc_label = lv_label_create(s_ctx.scr);
    lv_label_set_text(s_ctx.arc_label, "3");
    lv_obj_center(s_ctx.arc_label);
    lv_obj_align_to(s_ctx.arc_label, s_ctx.arc, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = lv_label_create(s_ctx.scr);
    lv_label_set_text(hint, "hold to confirm");
    lv_obj_set_style_text_color(hint, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_add_event_cb(s_ctx.scr, on_swipe, LV_EVENT_GESTURE, NULL);
}

static void open_on_lvgl(void *arg) {
    (void)arg;
    if (!lvgl_port_lock(0)) return;
    s_ctx.prev_screen = lv_screen_active();
    build_ui();
    lv_screen_load(s_ctx.scr);
    s_ctx.poll_timer = lv_timer_create(poll_tick, POLL_PERIOD_MS, NULL);
    lvgl_port_unlock();
}

void swap_screen_open(const swap_screen_args_t *args,
                      SemaphoreHandle_t        done_sem,
                      swap_ui_result_t        *out_result) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.args       = *args;
    s_ctx.done_sem   = done_sem;
    s_ctx.out_result = out_result;
    if (out_result) *out_result = SWAP_UI_CANCEL_TIMEOUT;
    lv_async_call(open_on_lvgl, NULL);
}
```

- [ ] **Step 5.2: Build**

```bash
pio run -e waveshare_esp32s3_28
```

- [ ] **Step 5.3: Add a `swap_show_demo` test verb**

In `swap.h`:

```c
// Diagnostic — opens the approval modal with hard-coded numbers and
// reports the resolution. Used by the host harness only.
const char *swap_show_demo_for_test(void);
```

In `swap.c`, append:

```c
#include "swap_screen.h"
#include "freertos/semphr.h"

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
```

In `testharness.c` add:

```c
// SWAP_DEMO  →  TEST OK confirm | cancel_release | cancel_swipe | cancel_timeout
if (strcmp(line, "SWAP_DEMO") == 0) {
    resp_ok(swap_show_demo_for_test());
    continue;
}
```

- [ ] **Step 5.4: Flash + smoke**

```bash
pio run -e waveshare_esp32s3_28 -t upload
python tests/run.py "SWAP_DEMO"
# Then physically: hold the screen 3s → expect TEST OK confirm
# Repeat: tap and release quickly → TEST OK cancel_release
# Repeat: swipe → TEST OK cancel_swipe
# Repeat: don't touch → after ~30s → TEST OK cancel_timeout
```

- [ ] **Step 5.5: Commit**

```bash
git add src/swap.h src/swap.c src/swap_screen.c src/testharness.c
git commit -m "swap: approval-modal LVGL screen with hold-to-confirm + cancels"
```

---

## Task 6: Jupiter `/swap` integration

**Files:**
- Modify: `src/swap.c`

- [ ] **Step 6.1: Add `jup_build_swap` helper**

In `swap.c`, after `jup_get_quote`:

```c
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
    out->priority_lamports = cJSON_IsNumber(pri) ? (uint64_t)pri->valuedouble : 1;
    cJSON_Delete(root);
    free(rsp);
    return out->tx_b64 != NULL;
}
```

- [ ] **Step 6.2: Build to confirm**

```bash
pio run -e waveshare_esp32s3_28
```

- [ ] **Step 6.3: Commit**

```bash
git add src/swap.c
git commit -m "swap: Jupiter v6 /swap integration"
```

---

## Task 7: v0 transaction signing (the splice)

**Files:**
- Modify: `src/swap.c` (parse + sign + splice)
- Modify: `src/testharness.c` (add `swap_test_sig` verb)
- Modify: `src/swap.h` (add diagnostic entry point)

This is the trickiest part. We **do not** parse address-lookup tables. Jupiter returns a wire-formatted v0 versioned transaction; we just locate the message bytes, find which signature slot is ours, sign the message, and splice the 64-byte signature into that slot.

**v0 wire format (relevant slice):**

```
[ compact-u16: numRequiredSignatures ]
[ numRequiredSignatures × 64-byte signature slots (zero-filled if unsigned) ]
[ message:
    [ 1 byte: prefix 0x80 (v0 marker bit) ]
    [ 1 byte: numRequiredSignatures (matches above) ]
    [ 1 byte: numReadonlySigned ]
    [ 1 byte: numReadonlyUnsigned ]
    [ compact-u16: numStaticAccountKeys ]
    [ numStaticAccountKeys × 32-byte pubkey ]      ← our pubkey lives here
    ... rest of message (recent blockhash, instructions, ALT refs)
  ]
```

The signers always occupy the **first** `numRequiredSignatures` slots of the static-account-keys array, and they're listed in the same order as the signature slots. So the signer at signature-slot `i` is `static_keys[i]`.

- [ ] **Step 7.1: Add base64 + compact-u16 helpers**

In `swap.c`, after the includes, add:

```c
#include "mbedtls/base64.h"
#include "wallet.h"

// compact-u16 (Solana's "shortvec"): 1-3 bytes, low 7 bits of each byte are
// data, MSB indicates continuation.
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
```

- [ ] **Step 7.2: Add the signer**

Append to `swap.c`:

```c
// Decode a base64 v0 versioned tx, locate the message bytes and the
// signature slot owned by our wallet, sign the message via wallet_sign(),
// and write the 64-byte signature into the slot. Re-encodes back to
// base64 in `out_b64` (NUL-terminated). Returns 0 on success, negative
// on any failure.
//
// Worst-case sizing: tx wire size for a Jupiter swap is typically ~1.2
// KB; 4 KB raw + 8 KB base64 covers everything we've ever observed.
#define SWAP_TX_RAW_CAP   4096
#define SWAP_TX_B64_CAP   8192

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
    if (msg_off >= raw_len)                                  { free(raw); return -1; }

    // Read message header: 1B prefix, 1B numReqSig, 1B numRoSig, 1B numRoUnsig
    if (msg_off + 4 >= raw_len) { free(raw); return -1; }
    cursor = msg_off + 4;
    uint16_t num_keys = 0;
    if (!compact_u16_read(raw, raw_len, &cursor, &num_keys)) { free(raw); return -1; }
    size_t keys_off = cursor;
    if (keys_off + (size_t)num_keys * 32 > raw_len)          { free(raw); return -1; }

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
```

- [ ] **Step 7.3: Add a self-test path**

In `swap.h`, add:

```c
// Diagnostic: take an unsigned base64 v0 tx, run sign_v0_tx_in_place,
// and verify the output decodes and the signature at our slot validates
// against the message bytes. Returns 0 on success, negative on failure.
int swap_test_sig_for_test(const char *in_b64, char *out_b64, size_t cap);
```

In `swap.c`, append:

```c
#include "ed25519.h"

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

    const uint8_t *mypub = wallet_pubkey_bytes();
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
```

- [ ] **Step 7.4: Add the test verb**

In `testharness.c`:

```c
// SWAP_TEST_SIG <b64-unsigned-v0-tx>
//   → TEST OK signed | TEST ERR rc=<n>
if (strncmp(line, "SWAP_TEST_SIG ", 14) == 0) {
    const char *in = line + 14;
    while (*in == ' ') in++;
    char *out = malloc(8192);
    if (!out) { resp_err("oom"); continue; }
    int rc = swap_test_sig_for_test(in, out, 8192);
    if (rc == 0) resp_ok("signed");
    else { char m[32]; snprintf(m, sizeof(m), "rc=%d", rc); resp_err(m); }
    free(out);
    continue;
}
```

- [ ] **Step 7.5: Build + flash**

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 7.6: Capture a real Jupiter unsigned tx and exercise**

In a host shell, fetch a real /quote+/swap response with the device's public key:

```bash
PUBKEY=$(python tests/run.py "WALLET_PUBKEY" | awk '{print $3}')

QUOTE=$(curl -s "https://quote-api.jup.ag/v6/quote?inputMint=EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v&outputMint=So11111111111111111111111111111111111111112&amount=100000&slippageBps=50")

TX=$(curl -s -X POST https://quote-api.jup.ag/v6/swap \
     -H 'content-type: application/json' \
     -d "{\"quoteResponse\":$QUOTE,\"userPublicKey\":\"$PUBKEY\",\"wrapAndUnwrapSol\":true,\"prioritizationFeeLamports\":1}" \
     | python -c 'import sys,json;print(json.load(sys.stdin)["swapTransaction"])')

python tests/run.py "SWAP_TEST_SIG $TX"
# Expected: TEST OK signed
```

(If `WALLET_PUBKEY` verb doesn't exist, add a one-liner to testharness.c that responds with `wallet_pubkey()`. It's a 5-line addition under `if (strcmp(line, "WALLET_PUBKEY") == 0)`.)

- [ ] **Step 7.7: Commit**

```bash
git add src/swap.h src/swap.c src/testharness.c
git commit -m "swap: v0-tx signature splice (no ALT resolution) + on-device test verb"
```

---

## Task 8: RPC submit + status poll

**Files:**
- Modify: `src/swap.c`

- [ ] **Step 8.1: Add submit + poll helpers**

Append to `swap.c`:

```c
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

    char rsp[512];
    char url[160];
    wallet_rpc_url(url, sizeof(url));
    bool ok = https_post_json(url, req, rsp, sizeof(rsp));
    free(req);
    if (!ok) return false;

    cJSON *root = cJSON_Parse(rsp);
    if (!root) return false;
    bool got = false;
    const cJSON *result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsString(result)) {
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
                bool landed = false;
                if (cJSON_IsArray(value) && cJSON_GetArraySize(value) > 0) {
                    const cJSON *st = cJSON_GetArrayItem(value, 0);
                    if (cJSON_IsObject(st)) landed = true;        // any status = landed
                }
                cJSON_Delete(root);
                if (landed) return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    return false;
}
```

Add to the includes:

```c
#include "freertos/task.h"
```

- [ ] **Step 8.2: Build to confirm**

```bash
pio run -e waveshare_esp32s3_28
```

- [ ] **Step 8.3: Commit**

```bash
git add src/swap.c
git commit -m "swap: sendTransaction + getSignatureStatuses polling"
```

---

## Task 9: `swap_request` orchestration

**Files:**
- Modify: `src/swap.c` (replace stub with full pipeline)

- [ ] **Step 9.1: Replace the stub `swap_request`**

Find the stub `swap_request(...)` from Task 1 and replace its body:

```c
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

    double scale_out  = pow10(b.decimals);
    double amount_out = (double)q.out_atomic    / scale_out;
    double min_out    = (double)q.min_out_atomic / scale_out;

    swap_screen_args_t ui = {0};
    strlcpy(ui.from_sym, a.sym, sizeof(ui.from_sym));
    strlcpy(ui.to_sym,   b.sym, sizeof(ui.to_sym));
    ui.amount_in    = amount_ui;
    ui.amount_out   = amount_out;
    ui.min_out      = min_out;
    ui.slippage_bps = slip;
    // Conservative fee estimate. The actual figure comes back from /swap
    // as priority_lamports, but we don't have that yet — show the upper
    // bound. ATA rent (0.00203928 SOL) is added if the destination ATA
    // isn't in the wallet's cache.
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
```

- [ ] **Step 9.2: Build**

```bash
pio run -e waveshare_esp32s3_28
```

- [ ] **Step 9.3: Commit**

```bash
git add src/swap.c
git commit -m "swap: orchestrate quote → approve → swap → sign → submit → confirm"
```

---

## Task 10: AI synthetic-tool integration

**Files:**
- Modify: `src/ai.c`

The synthetic tool is added to the OpenAI `tools` array unconditionally (no NVS toggle — it's a built-in capability). `execute_tool()` checks for the synthetic name **before** the normal x402 dispatch.

- [ ] **Step 10.1: Include the swap header**

At the top of `ai.c`:

```c
#include "swap.h"
```

- [ ] **Step 10.2: Append the synthetic tool inside `attach_tools()`**

Locate `attach_tools()` (around line 258 per the existing code). Find the `return` at the end of the function. **Just before** the return, insert:

```c
    // Built-in synthetic tool: on-device swap. Always exposed regardless
    // of which x402 services are enabled.
    if (!tools) tools = cJSON_AddArrayToObject(root, "tools");
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON *fn = cJSON_AddObjectToObject(t, "function");
        cJSON_AddStringToObject(fn, "name", "swap_tokens");
        cJSON_AddStringToObject(fn, "description",
            "Propose an on-device Solana swap between SOL, USDC, or any SPL "
            "token currently in the wallet. The user must physically hold "
            "the device touchscreen for 3 seconds to authorize. You cannot "
            "swap autonomously.");
        cJSON *p = cJSON_AddObjectToObject(fn, "parameters");
        cJSON_AddStringToObject(p, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(p, "properties");
        cJSON *pf = cJSON_AddObjectToObject(props, "from");
        cJSON_AddStringToObject(pf, "type", "string");
        cJSON_AddStringToObject(pf, "description",
            "Source token symbol — SOL, USDC, or a wallet SPL ticker.");
        cJSON *pt = cJSON_AddObjectToObject(props, "to");
        cJSON_AddStringToObject(pt, "type", "string");
        cJSON_AddStringToObject(pt, "description",
            "Destination token symbol — SOL, USDC, or a wallet SPL ticker.");
        cJSON *pa = cJSON_AddObjectToObject(props, "amount");
        cJSON_AddStringToObject(pa, "type", "number");
        cJSON_AddStringToObject(pa, "description",
            "Amount of `from` token in UI units (e.g. 0.5 = half a SOL).");
        cJSON *ps = cJSON_AddObjectToObject(props, "max_slippage_bps");
        cJSON_AddStringToObject(ps, "type", "integer");
        cJSON_AddStringToObject(ps, "description",
            "Optional max slippage in basis points (10..500). Omit for the default.");
        cJSON *req = cJSON_AddArrayToObject(p, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("from"));
        cJSON_AddItemToArray(req, cJSON_CreateString("to"));
        cJSON_AddItemToArray(req, cJSON_CreateString("amount"));
        cJSON_AddItemToArray(tools, t);
        n_added++;
    }
```

(`n_added` is the existing local counter in `attach_tools` — find its exact name and reuse.)

- [ ] **Step 10.3: Branch in `execute_tool` BEFORE the normal x402 dispatch**

In `execute_tool()`, immediately after the `(void)resolve_tool(...)` opening logic, add a synthetic-tool short-circuit. Insert right after the function's local-variable declarations and the initial `if (!resolve_tool(...))`. Concretely: replace the `if (!resolve_tool(...) ...)` block with this superset:

```c
static void execute_tool(const cJSON *services, const cJSON *enabled,
                         const char *tool_name, const char *args_json,
                         char *out, size_t cap) {
    // Synthetic built-ins first.
    if (strcmp(tool_name, "swap_tokens") == 0) {
        cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
        const cJSON *jf = args ? cJSON_GetObjectItem(args, "from")   : NULL;
        const cJSON *jt = args ? cJSON_GetObjectItem(args, "to")     : NULL;
        const cJSON *ja = args ? cJSON_GetObjectItem(args, "amount") : NULL;
        const cJSON *js = args ? cJSON_GetObjectItem(args, "max_slippage_bps") : NULL;
        if (!cJSON_IsString(jf) || !cJSON_IsString(jt) || !cJSON_IsNumber(ja)) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"bad_args\"}");
            if (args) cJSON_Delete(args);
            return;
        }
        uint16_t slip = cJSON_IsNumber(js) ? (uint16_t)js->valuedouble : 0;
        swap_result_t r;
        bool ok = swap_request(jf->valuestring, jt->valuestring,
                                ja->valuedouble, slip, &r);
        if (ok) {
            snprintf(out, cap,
                "{\"ok\":true,\"txid\":\"%s\",\"received\":%.6f,"
                "\"from\":\"%s\",\"to\":\"%s\"}",
                r.txid, r.amount_out, r.from_sym, r.to_sym);
        } else {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", r.error_msg);
        }
        if (args) cJSON_Delete(args);
        return;
    }

    // Existing x402 service dispatch.
    char  url[256];
    char  method[8];
    cJSON *params_schema = NULL;
    if (!resolve_tool(services, enabled, tool_name,
                      url, sizeof(url), method, sizeof(method), &params_schema)) {
        snprintf(out, cap, "{\"error\":\"unknown tool: %s\"}", tool_name);
        return;
    }
    /* ...rest of existing function unchanged... */
```

(Keep the rest of `execute_tool` exactly as it was — only the prologue changes.)

- [ ] **Step 10.4: Mention the tool in the system prompt**

In `build_system_prompt()` (or wherever `append_tool_listing` runs), add a stable line about `swap_tokens` so the LLM knows it exists even before any external services are configured. In `build_system_prompt`, just before the `append_tool_listing(...)` call, add:

```c
    n = strlen(out);
    snprintf(out + n, cap - n,
        "Built-in capability: swap_tokens(from, to, amount, max_slippage_bps?). "
        "Calls Jupiter on Solana for any pair among SOL, USDC, and the SPLs "
        "the wallet already holds. Every call shows an approval screen the "
        "user must hold for 3 seconds. Suggest swaps when useful; the user "
        "stays in control.\n");
```

- [ ] **Step 10.5: Build**

```bash
pio run -e waveshare_esp32s3_28
```

- [ ] **Step 10.6: Commit**

```bash
git add src/ai.c
git commit -m "ai: register synthetic swap_tokens tool + execute_tool dispatch"
```

---

## Task 11: Test-harness `swap_dry_run`

**Files:**
- Modify: `src/swap.h` (diagnostic entry)
- Modify: `src/swap.c`
- Modify: `src/testharness.c`

`swap_dry_run` exercises steps 1–4 (resolve, balance check, quote, open approval) without ever calling `/swap` or submitting a tx. Useful for fuzz-testing args + UI without spending fees.

- [ ] **Step 11.1: Add a dry-run entry to `swap.h`**

```c
// Diagnostic — runs the pipeline up to (but not past) the approval modal,
// returns the screen args as JSON. Caller is the test harness only.
bool swap_dry_run_for_test(const char *from_sym, const char *to_sym,
                           double amount_ui, uint16_t slippage_bps,
                           char *out_json, size_t cap);
```

- [ ] **Step 11.2: Implement in `swap.c`**

Append:

```c
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
    double scale = pow10(b.decimals);
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
```

- [ ] **Step 11.3: Add the verb**

In `testharness.c`:

```c
// SWAP_DRY_RUN <from> <to> <amount> [slippage_bps]
//   → TEST OK <json blob> | TEST ERR ...
if (strncmp(line, "SWAP_DRY_RUN ", 13) == 0) {
    char from[12] = {0}, to[12] = {0};
    double amount = 0;
    unsigned slip = 0;
    int parsed = sscanf(line + 13, "%11s %11s %lf %u",
                        from, to, &amount, &slip);
    if (parsed < 3) { resp_err("usage SWAP_DRY_RUN <from> <to> <amount> [slippage_bps]"); continue; }
    char buf[256];
    if (swap_dry_run_for_test(from, to, amount, (uint16_t)slip, buf, sizeof(buf))) {
        resp_ok(buf);
    } else {
        resp_err(buf);  // already JSON-shaped; harness OK with raw payload
    }
    continue;
}
```

- [ ] **Step 11.4: Build + flash**

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 11.5: Smoke**

```bash
python tests/run.py "SWAP_DRY_RUN USDC SOL 0.1"
# Expected: TEST OK {"from":"USDC","to":"SOL","in":0.100000,"out":0.000XXX, ...}
```

- [ ] **Step 11.6: Commit**

```bash
git add src/swap.h src/swap.c src/testharness.c
git commit -m "swap: SWAP_DRY_RUN harness verb (steps 1–4 without submit)"
```

---

## Task 12: End-to-end smoke

**Files:** none — this is a manual on-device validation.

- [ ] **Step 12.1: Flash latest firmware**

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 12.2: Top up wallet**

Ensure the device wallet holds at least **0.01 SOL** for fees + ATA rent and at least **1 USDC** for the swap.

- [ ] **Step 12.3: Trigger via chat**

Open the device's web UI (`http://<device-ip>/`) and send: *"Swap 0.1 USDC into SOL."*

Expected on-device:
- Creature screen yields to the SWAP modal showing:
  - `0.1 USDC` → `~0.000XXX SOL`
  - `min 0.000XXX SOL`
  - `slip 0.50%`
  - `fee 0.000005 SOL` (or 0.00204... if no SOL ATA was tracked, but it always is)

- [ ] **Step 12.4: Hold to confirm**

Hold the touchscreen continuously for 3 seconds. Arc fills, label counts 3→0, modal dismisses on completion.

Expected:
- ~5 seconds later, the wallet refresh fires → creature speaks the new balance via the existing `wallet_incoming_cb` path ("got 0.000XXX SOL") .
- Web UI chat shows the LLM acknowledging the swap.

- [ ] **Step 12.5: Cancel paths**

Repeat the swap request and verify each cancel mode:
- Tap and release immediately → modal dismisses, LLM gets `cancelled`
- Tap, then swipe → modal dismisses, LLM gets `cancelled`
- Don't touch for 30 s → modal dismisses, LLM gets `cancelled`

- [ ] **Step 12.6: Failure path**

With wallet empty, request a swap → expect the modal to NOT open; LLM receives `insufficient_balance` immediately.

- [ ] **Step 12.7: Final commit / no-op**

If everything passes, no code change. If anything breaks, fix and commit before merging.

---

## Self-review

**Spec coverage:**

- "On-device, Jupiter, hold-to-confirm 3s" → Tasks 5, 7, 8, 9 ✓
- "SOL/USDC + held SPLs" → Task 2 (resolution) ✓
- "Both AI and user can trigger" → Task 10 (single tool path) ✓
- "Slippage clamps + AI-overridable + shown" → Task 4 (clamp), Task 5 (display), Task 10 (arg) ✓
- "Approval screen: trust-essentials" → Task 5 ✓
- "Silent + creature reaction after confirm" → Task 9 calls `wallet_request_refresh` ✓
- "Quote freshness via /swap fetched on confirm" → Task 9 ordering ✓
- "Concurrency flag" → Task 9 `s_in_progress` ✓
- "Cancellation paths (release/swipe/timeout)" → Task 5 ✓
- "ATA rent in fee figure" → Task 9 ✓
- "v0 tx splice without ALT resolution" → Task 7 ✓
- "Error → tool-response shape" → Task 10 ✓
- "Test harness verb swap_dry_run" → Task 11 ✓
- "Manual smoke" → Task 12 ✓

**Placeholder scan:** No "TBD" / "TODO" / "implement later". Every step has actual code or an actual command.

**Type consistency:**
- `swap_token_t` defined Task 2; consumed by Tasks 3, 4, 9, 11 ✓
- `jup_quote_t` defined Task 4; consumed by Tasks 6, 9, 11 ✓
- `jup_swap_t` defined Task 6; consumed by Task 9 ✓
- `swap_screen_args_t` defined Task 1; consumed by Tasks 5, 9 ✓
- `swap_ui_result_t` defined Task 1; consumed by Tasks 5, 9 ✓
- `clamp_slippage` / `default_slippage` defined Task 4; consumed by Tasks 9, 11 ✓
- `pow10` forward-declared Task 4; defined same task; consumed Tasks 4, 9, 11 ✓
- `sign_v0_tx_in_place` defined Task 7; consumed Tasks 7 (test), 9 ✓
- `https_get` / `https_post_json` defined Task 4; consumed Tasks 4, 6, 8 ✓

All identifiers used in later tasks are defined in earlier ones.
