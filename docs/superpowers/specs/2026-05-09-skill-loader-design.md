# Generic Skill Loader (Markdown-Bodied Services)

**Status:** Draft, pending user review.
**Branch:** `feat/skill-loader`
**Date:** 2026-05-09
**Scope:** Let any third-party `skill.md` file (frontmatter + markdown body, e.g. the SP3ND agent skill) be uploaded to the device, toggled in the existing services UI, and used by the on-device AI to drive multi-step HTTP + Solana flows.

---

## Goal

Make the Daemon usable with arbitrary AI-agent skills published as markdown documents (the format used by SP3ND, OpenClaw, and similar). A user should be able to upload a `.md` file via the existing web UI, set any required credentials, toggle it on, and have the AI follow that skill's documented flow during voice conversations.

First skill target: SP3ND (https://sp3nd.shop), which lets the device buy products from Amazon and eBay using the device's USDC wallet via the x402 protocol.

---

## Why this is mostly an extension

The device already has the bones of a skill system:

- **Tool calling**: `src/ai.c:971-1001, 577-706` parses OpenAI `tool_calls` and dispatches them, with a "verdict" fast-path that splices a tool's response into the spoken reply.
- **Custom services**: `svc_custom` (4 KB NVS JSON), `svc_enabled` (toggle list), `/config` GET/POST, "Add Custom Service" web UI.
- **x402 wallet path**: `src/x402.c` signs USDC transfers with the device wallet against any 402 response.

A "service" today is shaped as `{id, name, description, baseUrl, endpoints: [{id, method, path, params, ...}]}`. Each enabled endpoint becomes a tool the AI can call. Toggles already work; the bake-in defaults (`daemon-fib`, `daemon-momentum`, etc.) live in `src/devcfg.c:61-128`.

This design extends that system rather than building a parallel "skills" concept.

---

## 1. Unified service shape

Two kinds of service entries coexist in the same `svc_custom` JSON array and the same `svc_enabled` toggle list. The `kind` field discriminates; absence implies structured (the existing default).

### A. Structured (existing, unchanged)

```json
{
  "id": "daemon-fib",
  "name": "Fib Levels",
  "description": "Fibonacci retracement levels",
  "baseUrl": "https://daemon-x402s-seven.vercel.app",
  "endpoints": [
    {
      "id": "analyze",
      "name": "Fib Retracement",
      "method": "GET",
      "path": "/api/fib",
      "params": [{"name": "symbol", "type": "string", "required": true}]
    }
  ]
}
```

Behavior unchanged. `attach_tools` (`src/ai.c:349`) emits one tool per endpoint named `x402_<fnv1a>_<ep>`. AI calls dispatch via `x402_call(baseUrl + path)`.

### B. Markdown (new)

```json
{
  "id": "sp3nd",
  "name": "SP3ND",
  "description": "Buy products from Amazon and eBay using USDC on Solana",
  "kind": "markdown",
  "version": "1.7.0",
  "credentials": ["SP3ND_API_KEY", "SP3ND_API_SECRET"]
}
```

Metadata stays in `svc_custom` (~200 bytes per skill). The markdown body lives in LittleFS at `/storage/skills/<id>.md` because 30 KB does not fit the 4 KB blob cap.

---

## 2. Skill markdown frontmatter

Required and optional fields:

```yaml
---
name: sp3nd                                # required, [a-z0-9_-]+, length <= 32, becomes the service id
description: Buy from Amazon & eBay        # required, length <= 200
version: 1.7.0                             # optional, free-form string, length <= 32
credentials:                                # optional list of env-style names
  - SP3ND_API_KEY
  - SP3ND_API_SECRET
---

# Markdown body of any length below the 64 KB cap.
```

Anything else (e.g. `metadata.openclaw.requires.env`) is ignored. The body below the closing `---` is stored verbatim in LittleFS. Convention: credential names should be skill-prefixed to avoid cross-skill collisions.

---

## 3. Storage

| Data | Location | Cap |
|---|---|---|
| Service metadata (both kinds) | `svc_custom` NVS blob | 4 KB total (existing); markdown entries are ~200 B each, max 16 markdown entries |
| Markdown body | LittleFS `/storage/skills/<id>.md` | 64 KB per skill, 1 MB total across all skills |
| Per-skill credentials | NVS namespace `skill_<id>`, one key per credential | 256 B per value |
| Toggle state | `svc_enabled` JSON array (existing) | 512 B (existing) |

LittleFS partition is the `storage` partition (~9 MB at 0x710000); 1 MB is conservative and forces hygiene. Mount happens at boot via `esp_vfs_littlefs_register`.

Per-skill credentials follow the existing devcfg pattern: load on `devcfg_init`, cached in RAM, save through immediately.

---

## 4. HTTP API

Three new routes added to the `httpd_uri_t` registration table in `src/server.c`:

| URI | Method | Purpose |
|---|---|---|
| `/skills` | POST | Body: raw markdown. Server parses frontmatter, validates, upserts metadata into `svc_custom`, writes body to LittleFS. Returns 200 + `{id, name, description, version, credentials, enabled}` JSON. |
| `/skills/{id}` | GET | Returns the raw markdown body (frontmatter + body) for the web UI's "view" affordance. |
| `/skills/{id}` | DELETE | Remove from `svc_custom`, remove from `svc_enabled`, delete LittleFS body, clear `skill_<id>` NVS namespace. Returns 200 + `{ok: true}`. |
| `/skills/{id}/credentials` | POST | Body: `{KEY: value, ...}`. Persist each KEY to `skill_<id>` NVS namespace. Returns 200 + `{set: ["SP3ND_API_KEY", ...]}`. |

Validation on POST `/skills`:

1. Body length <= 64 KB (else 413).
2. Frontmatter parses; `name` matches `^[a-z0-9_-]+$` and is <= 32 chars; `description` is non-empty and <= 200 chars.
3. After upsert, total `svc_custom` JSON <= 4 KB (else 413, with current usage in body).
4. Total LittleFS skills storage <= 1 MB (else 507, with current usage in body).
5. Total markdown-kind entries in `svc_custom` <= 16 (else 413).

Upsert semantics: re-uploading the same `name` overwrites body and updates metadata. NVS credential values for `credentials` that remain declared are preserved. Credentials no longer declared are pruned from the `skill_<id>` namespace.

Existing GET `/config` continues to enumerate all services (both kinds) under `customServices`. For markdown-kind entries, the `customServices` row carries an extra `credentials_set` array listing which declared credentials currently have values stored, so the web UI can render filled vs empty fields without ever exposing values.

POST `/config` may write structured services as today. It must reject markdown-kind entries (server returns 400 if a posted entry has `kind: "markdown"`); markdown entries are only created via POST `/skills`.

---

## 5. Web UI

The existing `/` HTML adds a sub-tab in the "Add Custom Service" panel:

- **Tab 1 (existing)**: structured form (name, description, baseUrl, endpoints).
- **Tab 2 (new)**: "Paste markdown / upload .md file". Textarea + file input. Submit POSTs raw body to `/skills`.

The unified services list shows both kinds. Markdown rows differ only in:

- An expand-arrow that reveals one masked text field per declared credential. "Save credentials" submits to `POST /skills/{id}/credentials`.
- A "View" link that opens the raw markdown in a modal (calls `GET /skills/{id}`).
- Delete button hits `DELETE /skills/{id}`.

Toggle switches use the existing `servicesEnabled` write path (POST `/config`), unchanged.

---

## 6. AI integration

Changes confined to `src/ai.c` plus a new module `src/skill_tools.c` / `src/skill_tools.h`.

### 6.1 System prompt injection

In `build_system_prompt` (`src/ai.c:208-293`), after the existing `append_tool_listing` call, iterate enabled markdown-kind services and append each one's body:

```
## Service: <name> (id: <id>)

<markdown body, frontmatter stripped>

---
```

All enabled markdown skills loaded every turn. Haiku 4.5 has the budget (200K context); intent-based lazy loading is a v2 concern.

If injection would push the system prompt past its fixed cap, the offending body is truncated with a `\n\n... [truncated]\n` marker.

### 6.2 Tool registration

In `attach_tools` (`src/ai.c:349`):

- Structured services keep producing per-endpoint tools as today.
- If at least one enabled service is markdown-kind, additionally register five generic tools (defined inline in `attach_tools` with hardcoded JSON Schema):
  - `http_request(method, url, headers, body)`
  - `x402_pay(payment_required, memo?)`
  - `secret_get(skill, name)`
  - `secret_set(skill, name, value)`
  - `solana_get_pubkey()`

If no markdown-kind service is enabled, the five tools are NOT registered (the prompt and tool list stay identical to today's behavior).

### 6.3 Tool dispatcher

In `execute_tool` (`src/ai.c:577-706`), extend the switch:

- Existing `x402_<hash>_*` route stays unchanged.
- New names route to handlers in a new module:

```c
// src/skill_tools.h
int skill_tool_http_request(const char *args_json, char *out_json, size_t out_cap);
int skill_tool_x402_pay   (const char *args_json, char *out_json, size_t out_cap);
int skill_tool_secret_get (const char *args_json, char *out_json, size_t out_cap);
int skill_tool_secret_set (const char *args_json, char *out_json, size_t out_cap);
int skill_tool_solana_pub (const char *args_json, char *out_json, size_t out_cap);
```

Each writes a JSON response into `out_json` and returns 0 on success or `ESP_ERR_*` on transport failure.

---

## 7. Tool semantics

### 7.1 `http_request(method, url, headers, body)`

- Args: `method` (GET/POST/PUT/DELETE), `url` (string), `headers` (object), `body` (string or object). All optional except `method` and `url`.
- Pre-processing: scan `url`, all `headers` values, and `body` (if string, plain replace; if object, walk all string leaves) for `${VAR}` references. For each `VAR`:
  - Find which enabled markdown-kind services declared `VAR` in their frontmatter `credentials`.
  - If exactly one declared it, look up the value in that skill's `skill_<id>` NVS namespace and substitute. Error if value unset.
  - If multiple enabled skills declared it: return `{error: "VAR is ambiguous: declared by <skill_a>, <skill_b>"}`.
  - If none declared it: return `{error: "VAR is not declared by any enabled skill"}`.
- Execute via the existing HTTPS path (the same one `x402_call` uses). Standard 402 responses are returned to the AI raw, body parsed into a JSON object if Content-Type is JSON.
- Return: `{status, headers, body}`. AI inspects `status` and decides whether to call `x402_pay`.

### 7.2 `x402_pay(payment_required, memo?)`

- Args: `payment_required` is the parsed `accepts[0]` object from a 402 response (carries `payTo`, `maxAmountRequired`, `asset`, `extra.feePayer`, `extra.order_number`, `resource`, etc.). Optional `memo` overrides the default.
- Default memo: if `payment_required.extra.order_number` is present, use the literal string `<service-name> Order: <order_number>`. Otherwise no memo instruction.
- Build a Solana VersionedTransaction:
  - `createTransferCheckedInstruction` for USDC (6 decimals) from device's ATA to `payTo`'s ATA, amount `maxAmountRequired`.
  - `createMemoInstruction(memo)` if memo is non-empty.
  - `feePayer = extra.feePayer` (the facilitator wallet pays gas, not the device).
  - `recentBlockhash` fetched fresh per call (per existing `project_x402_blockhash_uniqueness` memory: caching causes duplicate-signature errors).
- Sign with the device wallet keypair (existing wallet API).
- Determine facilitator URL: if `payment_required.extra.facilitator` is present, use it; else default to `https://facilitator.payai.network`.
- POST to `<facilitator>/verify` with x402 v1 payload `{x402Version: 1, scheme: "exact", network: "solana", payload: {transaction: <base64-tx>}}`. If invalid, return error.
- POST to `<facilitator>/settle` with the same payload. Return `{ok: true, signature, facilitator_response}` on success or `{ok: false, error}` on failure.

This requires a new `src/x402_pay.c` module that reuses `src/x402.c` primitives (blockhash fetch, ATA derivation, USDC transfer, memo) but adds external-feePayer + custom-memo + manual facilitator submission. The existing internal x402 client (used by chat itself) is untouched.

### 7.3 `secret_get(skill, name)` / `secret_set(skill, name, value)`

- `skill` must match an enabled markdown-kind service's id. Error otherwise.
- `name` must match `^[A-Z][A-Z0-9_]*$` and length <= 64.
- For `secret_get`: read from NVS namespace `skill_<skill>`, key `name`. Return `{ok: true, value}` or `{ok: false, error: "unset"}`.
- For `secret_set`: persist immediately. Returns `{ok: true}`. The web UI sees the new value on its next `GET /config` (the credential appears in `credentials_set`).
- Value length <= 256 bytes (matches NVS string default).

### 7.4 `solana_get_pubkey()`

- No args. Returns `{pubkey: "<base58>"}` from the existing wallet API.

---

## 8. Failure modes

| Symptom | Behavior |
|---|---|
| Upload missing required frontmatter field | 400 with explicit field name. |
| Upload exceeds 64 KB body cap | 413 with cap value. |
| Upload would push `svc_custom` past 4 KB | 413 with current size and cap. |
| Upload would push LittleFS skills storage past 1 MB | 507 with current usage and cap. |
| `${VAR}` unset at request time | Tool returns `{error: "VAR unset; call register / set via /skills/{id}/credentials"}`. |
| `${VAR}` declared by 2+ enabled skills | Tool returns `{error: "VAR is ambiguous: declared by <skill_a>, <skill_b>"}`. |
| Markdown body file missing on enabled skill (e.g. corrupted LittleFS) | Skip the skill at injection time, log a warning. AI does not see the skill's content but does see the generic tools (so other skills still work). |
| `x402_pay` verify or settle returns failure | `{ok: false, error}` returned to AI; AI decides whether to retry or surface to user. |
| `x402_pay` insufficient USDC | `{ok: false, error: "insufficient USDC"}`. AI surfaces to user. |
| Skill body > 64 KB at injection time (defensive) | Truncated with `... [truncated]` marker. |

---

## 9. Backward compatibility

- Existing structured services keep their exact shape, NVS keys, web UI, and per-endpoint tool generation. Zero migration required.
- Booting the new firmware on a device with an old `svc_custom` reads identically. No markdown-kind entries means the five generic tools are not registered, so the AI sees the same toolset as today.
- The existing 4-default-services preset, the bake-in defaults, and the bake-in-defaults reset logic in `src/devcfg.c:332-340` continue to work unchanged.

---

## 10. Test plan

1. **Unit (frontmatter parser)**: feed the SP3ND skill markdown, expect `name=sp3nd`, two credentials. Negatives: malformed YAML, missing `description`, oversize `name`.
2. **Upload**: `curl -X POST http://<device>/skills --data-binary @sp3nd.md`. Verify GET `/config` shows `sp3nd` with `kind=markdown`, `credentials=[SP3ND_API_KEY, SP3ND_API_SECRET]`, `credentials_set=[]`, `enabled=false`.
3. **Enable + register**: POST `/config` enabling `sp3nd`. `/say "register me with sp3nd"`. Expect AI to:
   - Call `solana_get_pubkey()` to get the device pubkey.
   - Call `http_request POST registerAgent` with the pubkey + an agent name.
   - Capture `api_key`/`api_secret` from response.
   - Call `secret_set(skill="sp3nd", name="SP3ND_API_KEY", value=...)` and the same for the secret.
   - Verify NVS namespace `skill_sp3nd` now has both keys.
4. **End-to-end purchase**: `/say "buy a $5 USB cable from Amazon US, ship to <address>"`. Expect AI to orchestrate: createPartnerCart, createPartnerOrder, payAgentOrder (gets 402), `x402_pay(<402 payload>)` (signs + verifies + settles), polls getPartnerOrders until status=Paid. Real money, real product. Smoke this against a $5 item to bound risk.
5. **Regression**: With sp3nd enabled, ask `/say "what's the fib level for SOL?"`. Existing `daemon-fib` tool path still works (structured tools unaffected by markdown coexistence).
6. **Storage limits**: upload 16 dummy markdown skills, then attempt a 17th. Expect 413. Upload a 70 KB markdown body. Expect 413.

---

## 11. Out of scope

- Intent-based lazy loading of skills into the system prompt (v2; current always-load is fine for a few enabled at once with Haiku 4.5).
- On-device LVGL "Skills" sub-screen (web UI only this iteration).
- Skill versioning / update notifications / signing.
- Sandboxing tool calls per skill (a markdown skill can request any URL today; trust model is "user only enables skills they trust").
- Multi-tenant credentials (one device, one set of credentials per skill).
- Streaming responses through `http_request` (response body capped at the existing chat-buffer size; long bodies are truncated for the AI to ask the user for narrower queries).

---

## 12. Open questions for review

1. Should `/skills` POST accept multipart for raw .md upload, or just a plain `text/plain` body with the markdown? (Spec assumes the latter.)
2. Should the system prompt include a short "skill catalog" header listing all enabled skill names + descriptions before each full body, to make the AI explicitly aware of the full set? (Spec leaves this to the markdown bodies themselves.)
3. Should `x402_pay` handle the SP3ND-specific Helius webhook polling (poll `getPartnerOrders` until status=Paid) inside the tool, or leave the AI to drive the polling loop via repeated `http_request` calls? (Spec leaves polling to the AI to keep `x402_pay` skill-agnostic.)

These are not blockers for an implementation plan; reviewer can flag any preference and the spec gets a one-line update.
