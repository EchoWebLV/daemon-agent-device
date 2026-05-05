# Pay.sh Dispatcher — Design

**Date:** 2026-05-05
**Branch:** `feat/pay-sh-dispatcher` (off `feat/faster-replies`)
**Status:** Approved through brainstorming; awaiting implementation plan

---

## Summary

Add a voice-activated paid-API dispatcher to Daemon. The user picks pay.sh
providers in the web admin; each enabled provider's endpoints become direct
LLM tools, registered alongside today's `swap_tokens`, `post_to_x`, and any
hand-added `devcfg_custom_services` entries. The main LLM calls them
natively — no internal sub-LLM routing — so latency matches the existing
swap/post tools.

A spending guardrail (auto / hold-confirm / refuse + daily cap) sits in
front of every paid x402 call, including current ones. A new
`pay_confirm_screen` reuses the swap-screen hold-to-confirm primitive.
The daily counter persists across reboots and resets at UTC midnight.

## Goals

- User picks pay.sh providers in the web admin; they appear immediately as LLM tools
- Voice command → main LLM → tool call → x402 payment → result spoken back
- Same speed as current swap/post tools — no router/caller sub-LLMs
- Spending controls cover **all** x402 calls (pay.sh, custom services, existing swaps)
- Hold-to-confirm modal with PTT-cancel for any single call above the auto cap
- Daily-spend counter persists across reboots and resets at UTC midnight
- Read-only "Spending" view on the on-device settings screen

## Non-goals

- MPP envelope support — standard x402 envelope only (additive ~150 LOC later if needed)
- `find_and_call` meta-tool with internal LLM dispatch (option C from brainstorm — deferred)
- Per-provider sub-spending caps (one global cap set applies)
- Multi-account / multi-user spending tracking
- Automatic catalog/openapi refresh on a schedule (manual via web admin)
- Parallel openapi fetching at boot (sequential is good enough for ≤20 providers)
- Web admin authentication (out of scope; same exposure as today)

## Architecture

```
┌── Web admin (browser) ─────────────────────────────────────┐
│  Fetches https://pay.sh/api/catalog directly               │
│  Renders 75 providers with price + description + category  │
│  Toggles per provider, sub-checkboxes per endpoint         │
│  "Save" → POST /api/services → devcfg_pay_enabled_services │
└────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌── Daemon boot ─────────────────────────────────────────────┐
│  payapi_init() task (PSRAM stack):                         │
│   • read enabled FQN list from devcfg                      │
│   • for each enabled provider, HTTPS GET openapi.json      │
│   • slim each endpoint → LLM tool definition               │
│   • register tools via existing ai.c attach_tools()        │
└────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌── User holds PTT, says "send mum an email saying I'm late" ─┐
│  STT → main LLM (sees curated tools natively)               │
│  LLM picks: pay_agentmail_send_msg_a7c3                     │
│  ai.c execute_tool() → x402_call_with_guard()               │
│   • phase 1: GET → 402 with payment-required envelope       │
│   • guard callback fires with actual price                  │
│   • policy check: auto / hold-confirm / refuse              │
│   • if confirmed: sign Solana USDC TransferChecked          │
│   • phase 2: retry with PAYMENT-SIGNATURE → 200             │
│  Record actual cost → devcfg_add_spend_today_micros()       │
│  Tool result → main LLM → TTS                               │
└─────────────────────────────────────────────────────────────┘
```

Three new translation units:

| Unit | Purpose |
|---|---|
| `payapi.c` / `.h` | Catalog fetch, openapi parse + slim, tool registration, guard impl, refresh ops |
| `pay_confirm_screen.c` / `.h` | Hold-to-confirm modal (LVGL, modeled on swap_screen) |

Plus targeted hooks in `x402.c`, `ai.c`, `devcfg.c`, `server.c`, `settings_screen.c`, `app_main.c`, and `html/index.html`.

### Interaction with the description cache

The in-flight WIP in [src/x402.c](../../../src/x402.c) caches the 402
description JSON per-URL so subsequent calls skip the phase-1 round
trip. The dispatcher inherits this benefit automatically: repeated calls
to the same enabled pay.sh provider go through the cache. Guard
semantics are unchanged — the cached envelope still carries the price,
so the guard sees it before signing.

If the server returns a fresh 402 (price changed under us), the cache
invalidates and the standard phase-1/phase-2 flow runs, with the guard
applied to the new price.

## Data model

### NVS keys (devcfg additions)

```c
// devcfg.h
const char *devcfg_pay_enabled_services(void);     // JSON, schema below
void        devcfg_set_pay_enabled_services(const char *json);

uint32_t devcfg_spend_auto_max_cents(void);        // default 10  ($0.10)
void     devcfg_set_spend_auto_max_cents(uint32_t v);
uint32_t devcfg_spend_confirm_max_cents(void);     // default 500 ($5.00)
void     devcfg_set_spend_confirm_max_cents(uint32_t v);
uint32_t devcfg_spend_daily_cap_cents(void);       // default 1000 ($10.00)
void     devcfg_set_spend_daily_cap_cents(uint32_t v);

uint64_t devcfg_spend_today_micros(void);          // running, UTC-day-rolled
void     devcfg_add_spend_today_micros(uint64_t delta);
```

**Why cents for caps and micro-USD for the counter?** Cents map cleanly
to a 2-decimal web admin input. The counter accumulates real call costs
which can be sub-cent (USDC has 6-decimal precision); micro-USD avoids
rounding bias against many small calls.

### Enabled-services JSON

```json
{
  "v": 1,
  "items": [
    { "fqn": "agentmail/email",
      "service_url": "https://x402.api.agentmail.to",
      "endpoints": ["POST /v0/inboxes/{id}/messages/send",
                    "GET /v0/inboxes"] },
    { "fqn": "merit-systems/stablecrypto/market-data",
      "service_url": "https://stablecrypto.dev",
      "endpoints": "all" }
  ]
}
```

`endpoints` is either an array of `"METHOD /path"` strings (curated
subset) or the string `"all"` (every endpoint from the openapi).
`service_url` is mirrored from the catalog at save time so boot doesn't
need to re-fetch the catalog just to look up base URLs.

### Daily-spend persistence blob

Stored as a single NVS blob `pay_spend_today`:

```c
typedef struct {
    int32_t  utc_day;     // time(NULL) / 86400 at last update
    uint64_t micros;      // accumulator
} spend_today_blob_t;
```

On read: if `utc_day < today` the blob is rewritten to `{ today, 0 }`
before returning the value. On `devcfg_add_spend_today_micros(delta)`:
same check, then `micros += delta`, then persist. Survives reboot.

## Web admin UX

Two new tabs in [src/html/index.html](../../../src/html/index.html).

### Services tab

```
┌─ Services ─────────────────────────────────────────────────┐
│ Tools enabled: 32 / 50  ████████░░░                        │
│                                                            │
│ [Refresh catalog] (last sync: 2m ago, 75 providers)        │
│                                                            │
│ ☑ AgentMail            messaging  $0–$10     ✓ 12 tools   │
│   ☑ POST /v0/inboxes/{id}/messages/send                    │
│   ☐ GET  /v0/inboxes                                       │
│   ☐ POST /v0/drafts                                        │
│   …                                                        │
│                                                            │
│ ☑ StableEnrich         data       $0.002–$0.44  ✓ 5 tools  │
│   ☑ POST /search/web                                       │
│   …                                                        │
│                                                            │
│ ☐ 2Captcha             devtools   $0.01      (off)         │
│ ☐ Crush Rewards        shopping   free       (off)         │
│ …                                                          │
│                                                  [Save]    │
└────────────────────────────────────────────────────────────┘
```

Behavior:
- Browser fetches `https://pay.sh/api/catalog` directly (CORS confirmed working in research).
- When a provider row is expanded, browser fetches `<service_url>/openapi.json` directly to enumerate endpoints.
- "Tools enabled" meter sums checked endpoints; soft cap 50 (warning above; no hard block).
- "✓ N tools" badge shows the **last device boot's** load result, fetched from `GET /api/services`.
- "Save" POSTs the full enabled JSON to `/api/services`. Device responds `{ "needs_reboot": false }` and re-runs `payapi_task` for newly added providers in the background.

### Spending tab

```
┌─ Spending ────────────────────────────────────┐
│ Auto-approve below     $ [0.10  ]             │
│ Hold-confirm below     $ [5.00  ]             │
│ (anything above hold-confirm is refused)      │
│ Daily cap              $ [10.00 ]             │
│                                               │
│ Today: $2.30 / $10.00  ████░░░░░░░  reset UTC │
│                                      [Save]   │
└───────────────────────────────────────────────┘
```

Behavior:
- Three editable fields, all in cents (2-decimal USD).
- "Refuse" isn't a separate threshold — it's whatever falls above the hold-confirm cap.
- Today's spend rendered live from `GET /api/spending` (poll every 5s while tab open).
- "Save" POSTs to `/api/spending`.

## On-device settings screen ([src/settings_screen.c](../../../src/settings_screen.c))

Add one row to the existing settings list, between Volume and Wifi:

```
Spending      $0.10 auto / $10.00 cap
              Today: $2.30 ████░░░░░░░
```

Tap opens a read-only detail screen showing the three caps, today's
spend, the per-provider load status, and an "edit on web at <ip>"
hint. View-only on-device by design; editing happens in the web admin
where typing dollar amounts on a phone keyboard beats fumbling LVGL
number wheels.

## Boot dispatcher

### Sequence

```
app_main → wifi_sta_init → ... → wifi_sta_got_ip event
                                       │
                                       ▼
                               payapi_init() [posts task]
                                       │
                                       ▼
                payapi_task (PSRAM stack 16KB, prio 4, core 0)
                  1. HTTPS GET /api/catalog → cache 12KB in PSRAM
                  2. Read devcfg_pay_enabled_services
                  3. For each enabled item (sequential, ~1s each):
                       HTTPS GET <service_url>/openapi.json
                       slim → tool defs (PSRAM)
                       record per-provider status
                  4. Hand tool-defs to ai.c registry
                  5. Sleep until wifi event or web-admin trigger
```

Sequential not parallel — predictable, simple, ~5 enabled providers ≈
5s background work. Doesn't block UI. If WiFi drops mid-flight: the
in-memory tool registry stays warm; calls just fail at request time
with the usual network error.

### Openapi slimming

Source: a 50-endpoint OpenAPI 3.1 spec is ~120KB raw.
Target: ~600 bytes per endpoint kept.

Per endpoint, keep:
- `method` (HTTP verb)
- `path` (route, including `{var}` params)
- One-line description = `summary` or first line of `description`
- Combined parameter schema:
  - Path params + query params → `properties` + `required`
  - Request body schema (after one `$ref` inline) → merged into `properties`
- Provider's catalog `min_price_usd` (for the price hint in the tool description)

Drop: `examples`, `tags`, `externalDocs`, `responses`, multiple-`$ref`
chains, `x-*` extensions, `securitySchemes` (we know it's x402).

Tool name format: `pay_<short_fqn>_<short_op>_<4-hex-hash>` — ≤64 chars
to fit OpenAI/Anthropic function-calling limits. Hash = first 4 hex of
FNV-1a(`fqn + method + path`); makes names stable across boots and
globally unique even if op IDs collide between providers.

### Tool registration

[src/ai.c](../../../src/ai.c)'s existing `attach_tools()` gets one new line:

```c
attach_tools(...) {
    ... existing built-in + devcfg_custom_services tools ...
    payapi_attach_tools(out_array);   // appends pay.sh-derived tools
}
```

`execute_tool()` already resolves a tool by name and dispatches. New helper:

```c
// payapi.h
typedef struct {
    const char *service_url;   // base URL from saved enabled list
    const char *method;        // HTTP method
    const char *path;          // template, may contain {var}
    uint32_t    price_usd_max_cents;  // ceiling from catalog
} payapi_tool_info_t;

bool payapi_resolve(const char *tool_name, payapi_tool_info_t *out);
```

If `payapi_resolve()` matches, `execute_tool()` calls
`x402_call_with_guard()` instead of plain `x402_call()`, passing
`payapi_guard()` and the resolved info as user-data.

### Refresh model

| Trigger | Effect |
|---|---|
| Boot | Fetch catalog + every enabled provider's openapi |
| Web admin "Refresh catalog" button | Catalog only |
| Web admin per-provider "Retry" link | That one provider's openapi |
| Web admin "Save" with new providers added | Newly-added providers' openapi (existing ones untouched) |

No periodic background refresh. Catalog changes rarely; user pulls when they want.

### Memory budget (boot peak)

| Buffer | Where | Size |
|---|---|---|
| Catalog cache | PSRAM heap | ~12 KB |
| Per-fetch openapi scratch | PSRAM heap (freed after slim) | up to 250 KB peak |
| Slimmed tool registry | PSRAM heap | ~30 KB at 50 tools |
| `payapi_task` stack | PSRAM | 16 KB |
| Internal RAM impact | — | **0** |

Peak PSRAM during boot ≈ 300 KB. Headroom on the 8 MB PSRAM is comfortable.

## Request-time flow

### Splitting x402_call into two phases

Today, [src/x402.c](../../../src/x402.c)'s `x402_call()` does GET → 402 →
sign → retry internally. We add a callback hook between 402-receive and
signature-build. The existing `x402_call()` becomes a 1-line wrapper
around the new function with a no-op guard, so all current callers keep
working unchanged.

```c
// x402.h additions
typedef enum {
    X402_GUARD_AUTO,            // silently pay
    X402_GUARD_CONFIRM_OK,      // user confirmed
    X402_GUARD_CONFIRM_DENIED,  // user denied
    X402_GUARD_REFUSE,          // policy refused (over daily cap, etc)
} x402_guard_decision_t;

typedef x402_guard_decision_t (*x402_guard_cb_t)(
    uint64_t    actual_micros,    // truth from 402 envelope
    const char *description,      // for modal text + voice
    void       *user);

x402_result_t x402_call_with_guard(
    const char *method, const char *url,
    const char *body, const char *auth,
    char *body_buf, size_t body_cap,
    x402_guard_cb_t guard, void *guard_user);
```

`x402_result_t` gains one field: `uint64_t cost_micros` (alongside the
existing `cost_usd`).

### Guard implementation (`payapi_guard()` in payapi.c)

```
on guard callback fired with (actual_micros, description):
   today  = devcfg_spend_today_micros()
   auto_  = devcfg_spend_auto_max_cents() * 10_000
   conf_  = devcfg_spend_confirm_max_cents() * 10_000
   daily_ = devcfg_spend_daily_cap_cents() * 10_000

   if today + actual > daily_:                  return REFUSE
   if actual ≤ auto_:                           return AUTO
   if actual ≤ conf_:
        if actual ≥ 1_000_000:        // ≥ $1.00
             tts_speak_blocking("<price> to <provider>. Hold to confirm.")
        decision = pay_confirm_screen_show_blocking(
                       actual, description, 30_000ms, ptt_gen)
        return decision  // CONFIRM_OK or CONFIRM_DENIED
   else:                                        return REFUSE
```

After successful paid response,
`devcfg_add_spend_today_micros(result.cost_micros)`.

### `pay_confirm_screen.c`

Modeled directly on [src/swap_screen.c](../../../src/swap_screen.c). Same
hold-to-confirm primitive (touch-IC hold detection ≈ 3s).

```
┌────────────────────────[X]────┐
│  Pay $0.42 to AgentMail?      │
│  Send email                   │
│                               │
│        ╭──╮                   │
│        │ 3│  hold              │
│        ╰──╯                   │
└───────────────────────────────┘
```

Cancel triggers (mirroring [swap_screen.c](../../../src/swap_screen.c) exactly — swipe is intentionally NOT a trigger because the CTP panel emits gesture events as capacitive jitter during a static hold):
- 3s continuous touch on the dial → `CONFIRM_OK`
- Touch released before 3s → `CONFIRM_DENIED` (release-cancel)
- Top-right `[X]` button tapped → `CONFIRM_DENIED`
- 30s with no touch → `CONFIRM_DENIED` (timeout)
- PTT button press → `CONFIRM_DENIED` (uses generation counter, cancels everything cleanly)

Voice cue: TTS speaks `"<price> to <provider>. Hold to confirm."` only
when actual price ≥ $1.00. Below that, modal opens silently — user must
look at the screen. Decision rationale: ambient "I want to use it
without thinking" mode for cheap things; loud-by-default for anything
that genuinely matters.

## LLM integration

### System prompt addition

Appended to `PERSONA` in [src/ai.c](../../../src/ai.c):

> You also have a set of paid services available as tools. Each tool's
> description ends with a price hint like "~$0.001 per call". Use them
> naturally when the user's request needs information or actions you
> can't fulfill on your own. Don't ask permission for cheap calls — the
> device handles spending caps automatically. If a paid call returns
> a `status` field (`declined` / `refused` / `error`), the user already
> saw the reason on the screen; just acknowledge it briefly.

### Tool description format (what the LLM actually sees)

```
pay_agentmail_send_msg_a7c3:
  AgentMail: send an email from your inbox.
  ~$0.001 per call. POST /v0/inboxes/{id}/messages/send
```

Price always at the start of the second sentence — gives the LLM a
stable cue when it has multiple options.

### Skip-LLM-2 fast path (existing pattern)

ai.c today bypasses the second LLM round when a tool result includes a
`verdict` field. Pay.sh tool results follow the convention:

| Outcome | Includes `verdict`? |
|---|---|
| Successful API call (data returned) | No — LLM summarizes the response |
| `status:"declined"` (user denied modal) | Yes — `"You declined the email."` |
| `status:"refused"` (over daily cap) | Yes — `"That'd put you over your daily limit."` |
| `status:"error"` (network/parse fail) | Yes — `"Couldn't reach AgentMail."` |

So failures stay snappy (one LLM round + preamble); successes get the
full summary round, same as today.

### Tool result shapes

```jsonc
// Success
<raw API response, truncated to 2KB>

// Declined / refused / errored — structured
{ "status": "declined" | "refused" | "error",
  "reason": "<machine code>",          // hold_to_confirm_denied | daily_cap_exceeded | over_max | network | parse | network_mismatch
  "price_usd": 0.42,
  "today_spent_usd": 9.82,
  "daily_cap_usd": 10.00,
  "verdict": "You declined the email." }
```

## HTTP endpoints (server.c additions)

| Method | Path | Body | Returns |
|---|---|---|---|
| GET  | `/api/services` | — | `{ items, providers: [{fqn, loaded, tool_count, error?, tried_at}], catalog_synced_at }` |
| POST | `/api/services` | `{ items: [...] }` | `{ ok: true }` or error |
| POST | `/api/services/refresh_catalog` | — | `{ ok: true, providers: 75 }` |
| POST | `/api/services/refresh_provider` | `{ fqn }` | `{ ok: true, tool_count: N }` |
| GET  | `/api/spending` | — | `{ auto_max_cents, confirm_max_cents, daily_cap_cents, today_micros, today_utc_day }` |
| POST | `/api/spending` | `{ auto_max_cents, confirm_max_cents, daily_cap_cents }` | `{ ok: true }` |

## File-level changes

| File | Change |
|---|---|
| `src/payapi.c` (new) | Catalog fetch, openapi parse + slim, tool registration, guard impl, refresh ops |
| `src/payapi.h` (new) | Public API |
| `src/pay_confirm_screen.c` (new) | Hold-to-confirm modal |
| `src/pay_confirm_screen.h` (new) | API |
| `src/x402.h` | Add guard typedef + `x402_call_with_guard()`; add `cost_micros` to `x402_result_t` |
| `src/x402.c` | Implement guard hook between 402-receive and signing; existing `x402_call()` becomes 1-line wrapper |
| `src/ai.c` | `execute_tool()` routes pay.sh tools through `x402_call_with_guard`; system prompt addition |
| `src/devcfg.h` | New keys (above) |
| `src/devcfg.c` | NVS reads/writes + UTC-day rollover for spend counter |
| `src/server.c` | Six new HTTP handlers (above) |
| `src/settings_screen.c` | "Spending" row + read-only detail screen |
| `src/html/index.html` | Services tab + Spending tab |
| `src/app_main.c` | Wire `payapi_init()` to `wifi_sta_got_ip` event |
| `CMakeLists.txt` | Register new source files |
| `docs/superpowers/specs/2026-05-05-pay-sh-dispatcher-design.md` | This file |

## Testing

| Layer | Test type | What |
|---|---|---|
| Slimmer | Unit (host) | Feed sample openapi.json files (AgentMail, StableEnrich, dtelecom), assert tool defs match golden snapshots |
| Guard decision | Unit (host) | Decision matrix: 8 cases (auto / confirm / refuse × under / over daily cap), assert correct enum |
| Daily counter | Unit (host) | UTC-day rollover: write at 23:59, read at 00:01, assert reset; reboot mid-day persists |
| HTTP endpoints | Integration (device) | POST/GET `/api/services` and `/api/spending` round-trip, persist across reboot |
| Hold-confirm modal | Hardware manual | Hold 3s → confirm, release < 3s → cancel, [X] tap → cancel, 30s timeout → cancel, PTT-press → cancel |
| End-to-end (free) | Hardware manual | Wire AgentMail / Crush Rewards / dtelecom (free tiers) via web admin, voice "send a test email" / "search shoes" / "transcribe this", verify result |
| End-to-end (paid) | Hardware manual | Smallest-price provider (~$0.001), real USDC, one call, verify on-chain transfer + counter increment |
| WiFi failure | Hardware manual | Kill WiFi between 402-receive and sign — verify no signature submitted |
| Concurrent PTT | Hardware manual | Trigger pay-confirm modal, press PTT during the 3s hold — verify generation-counter cancel propagates everywhere |

The hardware tests reuse the existing harness referenced in
`brainstorming/viability-analysis.md` ("16 tests, hardware harness")
rather than building a parallel one.

## Risks & open questions

- **Provider openapi quality varies.** Some specs are loose, missing required-field annotations, or use weird `$ref` chains. Slimmer must degrade gracefully — skip endpoints that won't slim cleanly, log to per-provider status, surface in web admin.
- **Tool description budget.** Function-calling schemas count toward LLM context. 50 endpoints × ~600B = 30KB. Combined with the main system prompt + wallet context + conversation history, total can push toward 60KB. Watch tokens-per-request and consider a hard cap if it bites.
- **Mainnet-only providers.** Some providers may live only on a sandbox/devnet. The 402 envelope tells us. If `network` ≠ Solana mainnet, `x402_call` returns error; tool result becomes `{"status":"error","reason":"network_mismatch"}`. Future enhancement: surface network in catalog metadata so we can flag mismatches in the web admin before saving.
- **Same op IDs across providers.** Tool name format includes a hash of `fqn + method + path` so collisions are impossible.
- **Web admin not authenticated.** Anyone on the LAN can change spending caps. Daemon's web admin already has this exposure; out of scope to fix here.
- **Cached description goes stale.** If a provider raises prices between cache fill and next call, the cached payment fails (server returns 402 again), the cache entry is invalidated, and the standard phase-1/phase-2 flow runs the guard against the fresh price. No money moves on stale cache.
- **TTS price announcement timing.** "Forty-two cents to AgentMail. Hold to confirm." takes ~2s to speak. The hold-confirm screen must already be visible by the time the TTS finishes so user action is meaningful. `payapi_guard()` opens the screen first, then issues the (non-blocking) TTS.
- **Provider removed from catalog.** A FQN saved in NVS may no longer exist in pay.sh's catalog at next boot. payapi_task still tries the saved `service_url`/openapi.json directly (we don't gate on catalog membership). If the URL still works, tools register normally; if it 404s, that provider goes into per-boot status as "✗ not found" and the user sees it in the web admin where they can untoggle it.
- **Provider unsaved while tools are mid-call.** If the user removes a provider in the web admin while an LLM tool call is in flight against one of its endpoints, the in-flight call completes (we hold a snapshot of the tool registry per request). The next boot or the immediate post-Save tool re-registration drops the unloaded provider's tools.

## Pay.sh research notes (appendix)

### Catalog endpoint
`GET https://pay.sh/api/catalog` — public, JSON, ~12KB, ~75 providers.
Per-provider fields: `fqn`, `title`, `description`, `use_case`,
`category`, `service_url`, `endpoint_count`, `has_metering`,
`has_free_tier`, `min_price_usd`, `max_price_usd`, `sha`.

### Openapi discovery
Each provider's `service_url` advertises Link headers:
`</openapi.json>; rel="service-desc"`,
`</llms.txt>; rel="https://llmstxt.org"`,
`</.well-known/api-catalog>; rel="api-catalog"`.

We fetch `<service_url>/openapi.json` directly. `llms.txt` would be
nicer (designed for LLM consumption) but isn't reliably present
(AgentMail returned a 404 in research).

### x402 wire envelope (what most providers use)
Standard Coinbase x402 spec:
- Header `payment-required` / `x-payment-required` / `www-authenticate`
- Body: base64-encoded JSON with `accepts[]` array, `payTo`,
  `maxAmountRequired`, `network` (e.g. `solana:5eykt4UsFv8P8NJdTREpY1vzqKqZKvdp`)

[src/x402.c](../../../src/x402.c) already parses this and signs Solana
USDC TransferChecked. No changes needed for envelope handling.

### MPP envelope (out of scope; deferred)
Some providers (notably the AAPL debugger at
`payment-debugger.vercel.app`) use RFC 7235
`WWW-Authenticate: Payment ...` header + RFC-9457
`application/problem+json` body:

```
WWW-Authenticate: Payment id="...", realm="...",
                  method="solana", intent="charge",
                  request="<base64 JSON>",
                  description="...", expires="..."
```

Decoded `request`:
```json
{ "amount": "10000",
  "currency": "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
  "methodDetails": { "decimals": 6, "feePayer": true,
                     "feePayerKey": "...", "network": "...",
                     "recentBlockhash": "...",
                     "tokenProgram": "Tokenkeg..." },
  "recipient": "..." }
```

Same underlying Solana USDC TransferChecked — semantically equivalent
to standard x402, just different wire framing. ~150 LOC additive to
x402.c when a provider we care about needs it.

### Wallet model
pay.sh CLI is Solana-native (`pay solana balance`,
`pay solana transfer`, `pay send 0.1 <recipient>`,
`pay account import ./keypair.json`). Daemon's existing Ed25519 wallet
is a direct match — no new key material needed.
