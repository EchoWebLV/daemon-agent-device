# Pay.sh Dispatcher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Read the spec end-to-end before starting; tasks point at existing files to mirror rather than inlining the code.

**Goal:** Voice-driven paid-API dispatcher. The user toggles pay.sh providers in the web admin; each enabled endpoint becomes a direct LLM tool, callable via the existing voice → STT → LLM → tool flow. A spending guardrail (auto / hold-confirm / refuse + UTC daily cap) hooks into `x402_call` between 402-receive and signature-build, covering all paid x402 calls.

**Architecture:** Web admin fetches `pay.sh/api/catalog` directly from the browser; on Save, a curated FQN list lands in NVS. On boot, `payapi_task` fetches each enabled provider's `openapi.json`, slims it into LLM tool definitions, and registers them through `ai.c::attach_tools()`. Request-time: `ai.c` routes pay.sh tools through `x402_call_with_guard()`; the guard reads spending caps from devcfg and either auto-pays, opens a `pay_confirm_screen` modal (mirrors `swap_screen`), or refuses. No internal sub-LLM dispatch.

**Tech Stack:** ESP-IDF + LVGL v9 + esp_http_client (mbedTLS) + cJSON for the firmware. Vanilla static HTML/JS for the web admin tabs. Python via `tests/run.py` for the E2E harness.

**Spec:** [docs/superpowers/specs/2026-05-05-pay-sh-dispatcher-design.md](../specs/2026-05-05-pay-sh-dispatcher-design.md)

**Pattern files (READ BEFORE STARTING):**
- [src/x402.c](../../../src/x402.c) — x402 client; we split this
- [src/swap_screen.c](../../../src/swap_screen.c) — modal pattern; clone for `pay_confirm_screen`
- [src/swap.c](../../../src/swap.c) — module pattern; clone for `payapi.c`
- [src/devcfg.c](../../../src/devcfg.c) — NVS getters/setters pattern
- [src/server.c](../../../src/server.c) — HTTP route registration pattern
- [src/ai.c](../../../src/ai.c) — `attach_tools` and `execute_tool` paths
- [src/testharness.c](../../../src/testharness.c) — TEST verb pattern
- [tests/run.py](../../../tests/run.py) — Python harness pattern

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/payapi.h` / `src/payapi.c` | NEW | Catalog fetch, openapi parse + slim, tool registration, guard impl, refresh ops |
| `src/pay_confirm_screen.h` / `src/pay_confirm_screen.c` | NEW | Hold-to-confirm modal; clone of `swap_screen.c` |
| `src/x402.h` / `src/x402.c` | EDIT | Guard typedef + `x402_call_with_guard()`; `x402_call()` becomes wrapper; `x402_result_t` gains `cost_micros` |
| `src/devcfg.h` / `src/devcfg.c` | EDIT | Six new keys (3 caps, enabled-services blob, today-spend blob, add-helper) |
| `src/server.c` | EDIT | Six new HTTP handlers (`/api/services` × 4, `/api/spending` × 2) |
| `src/ai.c` | EDIT | `execute_tool` routes pay.sh tools through guarded x402; PERSONA gets one paragraph |
| `src/settings_screen.c` | EDIT | "Spending" row + read-only detail screen |
| `src/html/index.html` | EDIT | Services tab + Spending tab |
| `src/app_main.c` | EDIT | `payapi_init()` on `wifi_sta_got_ip` |
| `src/CMakeLists.txt` | EDIT | Add `payapi.c` and `pay_confirm_screen.c` to SRCS |
| `src/testharness.c` | EDIT | New verbs: `PAYAPI`, `SPEND` |
| `tests/run.py` | EDIT | Cases for the new verbs + free-tier E2E |

---

## Verification approach

No host-side unit tests — only on-device E2E via the USB serial harness ([src/testharness.c](../../../src/testharness.c) ↔ [tests/run.py](../../../tests/run.py)). Per-task verification:

- **Compile clean:** `pio run`
- **Flash:** `pio run -t upload`
- **HTTP smoke:** `curl http://<device-ip>/api/<route>`
- **Harness verb:** in `pio device monitor`, type `BEGIN`, then `TEST <VERB ARGS>`, observe `TEST OK …`
- **Python harness:** `cd tests && python3 run.py`

Compile-clean is implicit on every code-touching task; listed only when it's the *only* check available.

---

# Phase A — Foundation

## Task 1: devcfg keys for spending caps + today's-spend blob

**Files:** EDIT `src/devcfg.h` and `src/devcfg.c`. Mirror the existing pattern (cached static + NVS read on `devcfg_init` + setter persists through).

Add prototypes per the spec's Data Model section:

```c
const char *devcfg_pay_enabled_services(void);
void        devcfg_set_pay_enabled_services(const char *json);
uint32_t devcfg_spend_auto_max_cents(void);     void devcfg_set_spend_auto_max_cents(uint32_t);
uint32_t devcfg_spend_confirm_max_cents(void);  void devcfg_set_spend_confirm_max_cents(uint32_t);
uint32_t devcfg_spend_daily_cap_cents(void);    void devcfg_set_spend_daily_cap_cents(uint32_t);
uint64_t devcfg_spend_today_micros(void);       void devcfg_add_spend_today_micros(uint64_t delta);
int32_t  devcfg_spend_today_utc_day(void);
```

Defaults: 10 / 500 / 1000 cents. Today blob: `{ int32_t utc_day; uint64_t micros }`. NVS key names ≤15 chars: `pay_enabled`, `spnd_auto_c`, `spnd_conf_c`, `spnd_daily_c`, `spnd_today`. UTC-rollover check on read AND on add — if `s_today.utc_day < time(NULL)/86400`, reset and persist before returning. Pre-2001 wall clock means time isn't synced; treat as "no rollover yet."

- [ ] Implement
- [ ] `pio run`
- [ ] Commit

## Task 2: x402 guard hook + `cost_micros` field

**Files:** EDIT `src/x402.h` and `src/x402.c`.

Add to header:

```c
typedef enum {
    X402_GUARD_AUTO, X402_GUARD_CONFIRM_OK,
    X402_GUARD_CONFIRM_DENIED, X402_GUARD_REFUSE,
} x402_guard_decision_t;

typedef x402_guard_decision_t (*x402_guard_cb_t)(
    uint64_t actual_micros, const char *description, void *user);

void x402_call_with_guard(const char *method, const char *url,
                          const char *json_body, const char *auth_bearer,
                          char *body_buf, size_t body_cap,
                          x402_guard_cb_t guard, void *guard_user,
                          x402_result_t *out);
```

Add `uint64_t cost_micros` to `x402_result_t`. In `x402.c`: rename current `x402_call` body to `x402_call_with_guard`, set `cost_micros` from the parsed `amount` integer, fire `guard` (if non-NULL) right BEFORE `solana_tx_build`. On `CONFIRM_DENIED` set `error="user_declined"`, status 402, jump to cleanup. On `REFUSE` same with `error="policy_refused"`. Existing `x402_call` becomes one line: `x402_call_with_guard(... NULL, NULL, out)`. Streaming variant gets no guard — out of scope. All existing callers unchanged.

- [ ] Implement (preserve description-cache WIP that lands later)
- [ ] `pio run`
- [ ] Commit

---

# Phase B — payapi core

## Task 3: payapi.h + skeleton

**Files:** CREATE `src/payapi.h`, `src/payapi.c`. EDIT `src/CMakeLists.txt` (add `payapi.c` to SRCS alphabetically near `x402.c`).

Public API per spec:

```c
void payapi_init(void);
bool payapi_refresh_catalog(void);
bool payapi_refresh_provider(const char *fqn);

typedef struct {
    const char *service_url; const char *method; const char *path;
    const char *fqn; uint32_t price_usd_max_cents;
} payapi_tool_info_t;
bool payapi_resolve(const char *tool_name, payapi_tool_info_t *out);

void payapi_attach_tools(struct cJSON *out_array);
x402_guard_decision_t payapi_guard(uint64_t actual_micros,
                                   const char *description, void *user);
struct cJSON *payapi_status_json(void);
```

`.c` is all stubs that ESP_LOGI their name and return safe defaults (false / AUTO / empty cJSON object).

- [ ] Implement
- [ ] `pio run`
- [ ] Commit

## Task 4: Catalog fetch + cache

**Files:** EDIT `src/payapi.c`.

In-memory `payapi_catalog_t = { items[], count, synced_at }` of slim provider rows: `{fqn, title, service_url, min_cents, max_cents, has_free_tier}`. PSRAM-backed via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.

`http_get_psram(url, cap)` helper — same pattern as `rpc_call` in [x402.c](../../../src/x402.c) but GET-only and `crt_bundle_attach` for TLS. 8s timeout. PSRAM scratch `cap=65536`.

`payapi_refresh_catalog()`: GET `https://pay.sh/api/catalog`, parse providers array, swap in under `s_catalog_mutex`. Free old `items` after swap.

`payapi_init()`: create mutex, spawn `payapi_task` with PSRAM stack 16KB on core 0 prio 4 (mirror `creature_screen.c`'s `xTaskCreatePinnedToCoreWithCaps` for `speech_task`). Task body for now: just `payapi_refresh_catalog()` then `vTaskDelete(NULL)`.

Add `TEST PAYAPI REFRESH_CATALOG` verb in `testharness.c` (responds `TEST OK ok=N`).

- [ ] Implement
- [ ] `pio run`, flash, in monitor: `BEGIN`, `TEST PAYAPI REFRESH_CATALOG` → `TEST OK ok=1` and log shows `catalog: 75 providers`
- [ ] Commit

## Task 5: OpenAPI fetch + slimmer

**Files:** EDIT `src/payapi.c`, `src/testharness.c`.

`payapi_tool_t = { name[64], fqn, service_url, method, path, description, params_schema_json, price_usd_max_cents }` — all PSRAM-allocated, owned. `s_tools[]` grows with `heap_caps_realloc`. `s_tools_mutex` guards.

Tool name builder: `pay_<short_fqn:≤12>_<short_op:≤12>_<4hex>` where `short_fqn`/`short_op` are lowercase alphanumeric (drop `/`, `-`, `_`); `op` is the last path segment; hash = first 4 hex of FNV-1a(`fqn|method|path`).

Slimmer (`slim_op_to_params_json`): walks one OpenAPI operation, merges path/query parameters and request-body schema (resolve one `$ref` hop into `components/schemas`) into a single `{type:"object", properties:{}, required:[]}` cJSON object, returns `cJSON_PrintUnformatted` string. Drop `examples`, `tags`, `responses`, `externalDocs`, `x-*`.

Description builder: `"<title>: <summary>. <price hint>. <METHOD> <path>"`. Price hint = `"free"` if `has_free_tier && min_cents==0`, else `~$0.XXX per call`.

`payapi_refresh_provider(fqn)`:
1. Find provider in catalog (under `s_catalog_mutex`); copy `service_url`/title/min/max into local.
2. GET `<service_url>/openapi.json` with cap 256KB.
3. Parse `paths.*.{get,post,put,patch,delete}`. For each operation, check the enabled-services JSON (`endpoints` is `"all"` or `["METHOD /path", ...]`).
4. Under `s_tools_mutex`: drop existing tools for this fqn, then append new ones.

Update `payapi_task` to walk enabled-services after the catalog refresh and call `payapi_refresh_provider(fqn)` for each.

Add `TEST PAYAPI REFRESH_PROVIDER <fqn>` verb.

- [ ] Implement
- [ ] `pio run`
- [ ] On device: `TEST PAYAPI REFRESH_PROVIDER agentmail/email` → `TEST OK ok=…`; log shows `agentmail/email: registered N tools`
- [ ] Commit

## Task 6: Tool registration handoff to ai.c

**Files:** EDIT `src/payapi.c`, `src/ai.c`.

In `payapi.c`, replace the three stubs:
- `payapi_attach_tools(out_array)`: under `s_tools_mutex`, for each tool append `{type:"function", function:{name, description, parameters: parse(params_schema_json)}}` to `out_array`.
- `payapi_resolve(name, out)`: linear scan `s_tools[]`, populate `out` with const pointers into the registry; return false on miss.
- `payapi_status_json()`: build `{providers:[{fqn,loaded,tool_count}], catalog_synced_at}` from the enabled-services + registry intersection.

In [ai.c](../../../src/ai.c): include `payapi.h`. In `attach_tools` after the existing custom-services loop, append `payapi_attach_tools(<tools-array-var>)` — find the actual variable name in the function (likely `tools_array` or `arr`).

- [ ] Implement
- [ ] `pio run`
- [ ] Commit

## Task 7: Guard with auto + refuse arms (modal lands T12)

**Files:** EDIT `src/payapi.c`, `src/testharness.c`.

`payapi_guard(actual_micros, description, user)`:
- Read three caps × 10000 → micros.
- Read `today = devcfg_spend_today_micros()`.
- `today + actual > daily` → `REFUSE`
- `actual ≤ auto` → `AUTO`
- `actual ≤ confirm` → for now `REFUSE` (with TODO comment pointing at Task 12)
- else `REFUSE`

Add testharness verbs:
- `TEST PAYAPI GUARD <micros>` → `TEST OK decision=<AUTO|CONFIRM_OK|CONFIRM_DENIED|REFUSE>`
- `TEST SPEND TODAY` → `TEST OK micros=<n>`
- `TEST SPEND ADD <micros>` → `TEST OK added`

- [ ] Implement
- [ ] On device, run the eight-case decision matrix from the spec's testing table. All combos must match expected enum.
- [ ] Commit

---

# Phase C — HTTP endpoints

## Task 8: `/api/spending` GET + POST

**Files:** EDIT `src/server.c`. Mirror existing handler/registration style (look for `/notify` and `/say` handlers from MCP work).

GET returns: `{auto_max_cents, confirm_max_cents, daily_cap_cents, today_micros, today_utc_day}`.

POST body: `{auto_max_cents, confirm_max_cents, daily_cap_cents}`. Each field optional; only present ones are set. Reply `{"ok":true}`.

- [ ] Implement
- [ ] `pio run -t upload`. Then: `curl http://<ip>/api/spending` → JSON; `curl -X POST -H "Content-Type: application/json" -d '{"auto_max_cents":25}' http://<ip>/api/spending` → `{"ok":true}`. Re-GET shows the change. Reboot, re-GET — value persists.
- [ ] Commit

## Task 9: `/api/services` GET, POST, refresh_catalog, refresh_provider

**Files:** EDIT `src/server.c`.

GET returns: `{items: <enabled list>, providers: <status array>, catalog_synced_at}`.

POST body: `{v:1, items:[{fqn, service_url, endpoints}]}`. Persist via `devcfg_set_pay_enabled_services`, then synchronously call `payapi_refresh_provider(fqn)` for each item in the list (existing/already-loaded providers are dropped-and-re-registered cheaply; new ones get fetched). Reply `{"ok":true}`. Body cap 16KB.

`POST /api/services/refresh_catalog` — calls `payapi_refresh_catalog()`.
`POST /api/services/refresh_provider` body `{fqn}` — calls `payapi_refresh_provider(fqn)`.

- [ ] Implement
- [ ] `curl http://<ip>/api/services` → JSON; `curl -X POST -H "Content-Type: application/json" -d '{"v":1,"items":[{"fqn":"agentmail/email","service_url":"https://x402.api.agentmail.to","endpoints":"all"}]}' http://<ip>/api/services` → `{"ok":true}`. Re-GET shows `tool_count > 0`.
- [ ] Commit

---

# Phase D — ai.c integration

## Task 10: Route pay.sh tools through guarded x402 + system prompt

**Files:** EDIT `src/ai.c`.

In `execute_tool` (around line 577 per the spec's research), BEFORE the existing custom-x402 dispatch:

```c
payapi_tool_info_t info;
if (payapi_resolve(tool_name, &info)) {
    char url[1024]; const char *body=NULL; char *body_owned=NULL;
    build_url_with_params(info.service_url, info.path, info.method,
                          args_json, url, sizeof url, &body_owned, &body);
    x402_result_t r = {0};
    x402_call_with_guard(info.method, url, body, NULL,
                         tool_response_buf, TOOL_RESPONSE_CAP,
                         payapi_guard, NULL, &r);
    free(body_owned);
    if (r.status >= 200 && r.status < 300) {
        devcfg_add_spend_today_micros(r.cost_micros);
        return tool_result_with_body(tool_response_buf);
    }
    return tool_result_failure_json(&r, info.fqn);
}
```

`build_url_with_params(base, path, method, args, url[], cap, **body_owned, **body)`: substitute `{name}` from args left-to-right; remaining args go on query string for GET/DELETE or as a JSON body for POST/PUT/PATCH (body owned by caller). Mirror the URL-encoding helper that already exists for the custom-services path.

`tool_result_failure_json(&r, fqn)` returns the structured shape from spec §"Tool result shapes":
- `r.error == "user_declined"` → status `declined`, reason `hold_to_confirm_denied`, verdict `"You declined the call to <fqn>."`
- `r.error == "policy_refused"` → status `refused`, reason `daily_cap_exceeded_or_over_max`, verdict `"That'd put you over your spending limits."`
- else → status `error`, reason from `r.error` or `"network"`, verdict `"Couldn't reach <fqn>."`
Always include `price_usd`, `today_spent_usd`, `daily_cap_usd`.

Append the spec's PERSONA paragraph to the existing `PERSONA` string (verbatim from spec §LLM integration).

- [ ] Implement
- [ ] `pio run`
- [ ] Commit

---

# Phase E — pay-confirm modal + voice cue

## Task 11: pay_confirm_screen modal

**Files:** CREATE `src/pay_confirm_screen.h` and `src/pay_confirm_screen.c`. EDIT `src/CMakeLists.txt`.

Header exposes one function:

```c
x402_guard_decision_t pay_confirm_screen_show_blocking(
    uint64_t actual_micros, const char *description, uint32_t ptt_gen);
```

Implementation: clone [src/swap_screen.c](../../../src/swap_screen.c) line for line — same 50Hz LVGL timer, same 3000ms hold accumulator, same release-cancel + 30s no-touch cancel + top-right `[X]` button cancel. Differences:
- Title `"Pay $%.2f"` from `actual_micros / 1e6`
- Sub-line = `description`
- Add a check in `poll_tick`: `if (s_ptt_gen != ptt_gen0) close_with(DENIED)` — `s_ptt_gen` is `extern uint32_t` declared in `creature_screen.c`
- Build modal via `lv_async_call` (caller is non-LVGL task), block on a binary semaphore until `close_with` releases it
- NO swipe handler — explicitly removed in swap_screen because of CTP jitter; spec §request-time flow notes this

- [ ] Implement (read swap_screen.c carefully — copy its style choices exactly)
- [ ] `pio run`
- [ ] Add temporary harness verb `TEST PAYCONF <micros>` that calls `pay_confirm_screen_show_blocking(micros, "harness", s_ptt_gen)` and prints the decision. Verify all five cancel paths: hold→OK, release→DENIED, [X]→DENIED, 30s→DENIED, PTT→DENIED. Remove the harness verb.
- [ ] Commit

## Task 12: Guard upgrade — modal + TTS cue

**Files:** EDIT `src/payapi.c`.

Replace the `actual ≤ confirm` arm in `payapi_guard`:
1. If `actual_micros >= 1_000_000` (≥ $1.00), enqueue TTS via `voice_speak_chunk`: `"<dollar amount> to confirm. Hold the screen."` (non-blocking; modal opens immediately, TTS catches up).
2. Return `pay_confirm_screen_show_blocking(actual_micros, description, s_ptt_gen)`.

Include `pay_confirm_screen.h`, `voice.h`, declare `extern uint32_t s_ptt_gen;`.

- [ ] Implement
- [ ] `pio run`, flash, on device: `TEST PAYAPI GUARD 200000` → modal appears (no TTS). Hold/release as appropriate. `TEST PAYAPI GUARD 1500000` → TTS speaks `"1.50 to confirm…"` then modal appears.
- [ ] Commit

---

# Phase F — Web admin UI

## Task 13: Spending tab

**Files:** EDIT `src/html/index.html`. Mirror the SOCIALS tab pattern from x-posting.

Tab content: three number inputs (auto/confirm/daily, step 0.01, min 0), today's progress bar, Save button.

Script:
- `loadSpending()`: fetch `/api/spending`, populate inputs (cents → 2-decimal USD), render today/cap + progress %
- `saveSpending()`: POST `/api/spending` with all three caps as cents
- 5s polling interval while tab is visible (start on activate, clear on deactivate)

- [ ] Implement
- [ ] Flash, open `http://<ip>/`, click Spending tab. Edit auto, Save → status flips to "Saved." Reload page — value persists.
- [ ] Commit

## Task 14: Services tab

**Files:** EDIT `src/html/index.html`.

Tab content: "Refresh catalog" button, tools-enabled meter, scrollable provider list, Save button.

Script:
- `loadServicesState()`: fetch `/api/services` (enabled + statuses) AND `https://pay.sh/api/catalog` (full provider list, CORS works). Render rows.
- Per row: checkbox + `"<title>  <category>  $min–$max  ✓N tools | ✗"` label. Expanded rows fetch `<service_url>/openapi.json` lazily (browser-side, cached on the provider object as `_openapi`).
- Per-endpoint sub-checkboxes inside expanded rows. State machine for `endpoints`: `"all"` if all checked; explicit array otherwise.
- Tools-enabled meter sums checked endpoints; soft cap 50 (label shows `N / 50`, no hard block).
- `saveServices()`: POST `/api/services` with `{v:1, items:[...]}`.
- "Refresh catalog" button POSTs `/api/services/refresh_catalog`, then reloads.

- [ ] Implement
- [ ] Flash, open Services tab. Enable AgentMail, Save. Reboot, reopen — selection persists, ✓ badge shows next to AgentMail.
- [ ] Commit

---

# Phase G — On-device settings screen

## Task 15: Spending row + read-only detail

**Files:** EDIT `src/settings_screen.c`.

Add a row between Volume and Wifi (or wherever fits the existing list order). Label `Spending`, value `"$0.10 auto / $10.00 cap"` from `devcfg_spend_auto_max_cents` and `devcfg_spend_daily_cap_cents`. Click handler opens a detail screen.

Detail screen: full-screen LVGL container, shows the three caps + today's micros + the daily cap, plus `"Edit on web at <ip>"` hint and a Back button. Read-only — no inputs, no save.

`settings_screen_refresh()` updates the row's value label so it stays current after web-admin saves.

- [ ] Implement (mirror existing rows in `settings_screen.c` for layout/style)
- [ ] Flash, navigate to Settings → see the row → tap → see detail → tap Back → return.
- [ ] Commit

---

# Phase H — Boot wire-up

## Task 16: `payapi_init()` on wifi-up

**Files:** EDIT `src/app_main.c`.

Find the existing wifi-connected callback (the place that today starts the HTTP server / mDNS / etc on `wifi_sta_got_ip`). Add `payapi_init();` after the existing body. Include `payapi.h`.

- [ ] Implement
- [ ] `pio run -t upload && pio device monitor`. After wifi connects: log shows `payapi: catalog: 75 providers` and one `registered N tools` line per enabled provider.
- [ ] Commit

---

# Phase I — Tests + manual E2E

## Task 17: Python harness cases + manual E2E checklist

**Files:** EDIT `tests/run.py`.

Add cases (mirror existing case style, register in the same CASES table):

- `test_payapi_catalog_refresh`: `TEST PAYAPI REFRESH_CATALOG` → `OK ok=1`
- `test_payapi_provider_refresh`: `TEST PAYAPI REFRESH_PROVIDER agentmail/email` → response includes `fqn=agentmail/email`
- `test_payapi_guard_auto`: under default caps, `TEST PAYAPI GUARD 50000` → `decision=AUTO`
- `test_payapi_guard_refuse_over_confirm`: `TEST PAYAPI GUARD 6000000` → `decision=REFUSE`
- `test_spending_endpoints_round_trip`: GET `/api/spending`, POST new caps, GET again, assert change. Use `urllib.request`.

Manual E2E checklist (record results in commit body):

1. **Free-tier path:** enable AgentMail in web admin → voice "create a test inbox" → LLM uses tool → response spoken. No modal.
2. **Hold-confirm path:** drop auto cap to $0.0001 → enable a $0.01 provider → voice a request → modal opens → hold 3s → call completes. Run again, release early → "You declined…" verdict spoken.
3. **TTS cue threshold:** enable any provider that costs ≥ $1 → voice a request → TTS speaks the price BEFORE the modal opens. Repeat with a < $1 call → modal silent, no TTS.
4. **Daily-cap refusal:** set daily cap = today's-spend + small headroom → voice a request that would exceed → "That'd put you over your spending limits." spoken. Counter unchanged.
5. **PTT mid-modal:** trigger a hold-confirm modal → press PTT → modal closes with DENIED, generation counter cancels everything cleanly.
6. **Reboot persistence:** run a paid call (counter increments) → reboot → settings detail screen shows the same today value → wait for UTC midnight (or set system clock) → counter resets to 0.

- [ ] Implement Python cases
- [ ] `cd tests && python3 run.py` — full suite green
- [ ] Run all six manual checks; record outcomes
- [ ] Commit

---

# Wrap-up

After Task 17:

- [ ] `pio run` once more — clean
- [ ] `git log --oneline feat/faster-replies..HEAD` — every message reads cleanly
- [ ] `git push -u origin feat/pay-sh-dispatcher`
- [ ] Open the PR; link the spec as the description

The description-cache WIP from `feat/faster-replies` is still uncommitted on this branch. Decide whether to fold it into this PR (the cache shortcuts phase-1; the guard cleanly handles cached envelopes via the existing price field) or split it.
