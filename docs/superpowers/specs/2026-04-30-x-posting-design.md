# Voice-Triggered X Posting with On-Device Approval

**Status:** Approved design, ready for implementation plan.
**Branch:** `feat/x-posting`
**Date:** 2026-04-30
**Scope:** Let users connect their X (Twitter) account, draft posts via voice, and approve/reject them on the device LCD before they're sent.

---

## Goal

Voice in, X post out, with the user explicitly approving each draft on-device before it goes live. Multi-tenant — each Daemon owner connects their own X account. No central server holds tokens; the device owns its credentials end-to-end.

---

## Architecture

Pattern mirrors the existing **swap** subsystem:

| Concern | Module |
|---|---|
| Pairing flow + network calls + token lifecycle | `src/social_x.c` / `src/social_x.h` |
| Approval modal (LVGL) | `src/x_post_screen.c` / `src/x_post_screen.h` |
| Web UI tab + server endpoints | additions to `src/server.c` and `src/html/index.html` |
| OAuth bounce page | new static route in the existing `x402-bundle` Vercel repo |

The voice path reuses the existing pipeline: `mic → stt → ai (LLM with tool-calls)`. Posting is exposed to the LLM as a new tool, `post_to_x`, that **drafts but does not send**. The handler queues the draft, opens the approval modal, and only sends on user confirmation.

---

## OAuth model

X API v2 with OAuth 2.0 Authorization Code + **PKCE for public clients** — no client secret. Only the public `client_id` ships in firmware.

### Pairing flow

```
1. User opens device web UI → SOCIALS tab → clicks "Connect X"
2. Web UI calls POST `/social/x/begin`. Device generates:
   - `code_verifier` (random 64 bytes, base64url-encoded)
   - `code_challenge = SHA256(verifier)`, base64url-encoded
   - Stores verifier in a single RAM slot (only one pairing at a time; new "begin" overwrites any stale slot)
3. `/social/x/begin` returns the full auth URL. Web UI opens it in a new tab:
     https://x.com/i/oauth2/authorize?
       response_type=code&
       client_id=<DAEMON_X_CLIENT_ID>&
       redirect_uri=https://<x402-bundle-host>/x-callback&
       scope=tweet.read+tweet.write+users.read+offline.access&
       code_challenge=<challenge>&
       code_challenge_method=S256&
       state=<8-byte random hex, also stored in RAM>
4. User authorizes on x.com
5. X redirects to `https://<x402-bundle-host>/x-callback?code=<auth_code>&state=<state>`
6. Bounce page renders the `code` in big text with a Copy button
7. User pastes the code into the device web UI's "Paste code" field
8. Web UI POSTs `/social/x/finish` with `{ "code": "..." }`. The device uses its in-RAM verifier (only one pairing slot) and POSTs https://api.x.com/2/oauth2/token with:
     grant_type=authorization_code
     code=<auth_code>
     code_verifier=<verifier from RAM>
     client_id=<DAEMON_X_CLIENT_ID>
     redirect_uri=https://<x402-bundle-host>/x-callback
9. Response: { access_token, refresh_token, expires_in, scope }
10. Device persists tokens (see Storage) + queries `GET /2/users/me` for the handle, persists handle
11. Device clears the RAM verifier slot
12. Web UI polls `/social/x/state`, flips to "Connected as @handle"

The `state` parameter is generated and verified per OAuth-2.0 best practice. Since pairing is local-network only and there's only one slot, it doesn't need to disambiguate between concurrent pairings — but it does protect against a stale browser tab finishing pairing into a new session.
```

The verifier never leaves the device. The bounce page is stateless — it doesn't store or forward the code, only displays it.

### Refresh

When `social_x_post()` gets a 401, it tries the refresh-token grant once:
```
POST https://api.x.com/2/oauth2/token
grant_type=refresh_token
refresh_token=<stored>
client_id=<DAEMON_X_CLIENT_ID>
```
On success, persist the new pair (X rotates refresh tokens on each use). On failure, clear the stored tokens and return an error code that surfaces "Reconnect on the web UI" via TTS.

### Disconnect

Web UI POST `/social/x/disconnect`:
1. Best-effort: POST to `https://api.x.com/2/oauth2/revoke` with current `access_token`
2. Always: clear NVS keys
3. Web UI flips back to "Connect X"

---

## On-device approval modal

Implementation: **`src/x_post_screen.c` / `src/x_post_screen.h`**, mirroring `swap_screen.c` exactly.

### Public API

```c
typedef struct {
    char text[300];        // draft post (X cap is 280; 300 leaves slack for safety)
    char handle[20];       // @handle from /2/users/me, for "Posting as @yordan"
} x_post_screen_args_t;

typedef enum {
    X_POST_UI_CONFIRM,
    X_POST_UI_CANCEL_RELEASE,
    X_POST_UI_CANCEL_SWIPE,
    X_POST_UI_CANCEL_TIMEOUT,
} x_post_ui_result_t;

void x_post_screen_open(const x_post_screen_args_t *args,
                        SemaphoreHandle_t          done_sem,
                        x_post_ui_result_t        *out_result);
```

### UX

Layout on 320×240 panel, FACE_Y_OFFSET applies same as elsewhere:

```
┌──────────────────────────────────┐
│ Post to X?           @handle     │  status bar
├──────────────────────────────────┤
│                                  │
│   Today we shipped the           │
│   Daemon's pixel-art creature    │  draft text — wraps,
│   designer. Seven faces, all     │  paginates if needed
│   drawable in-browser.           │
│                                  │
│                                  │
├──────────────────────────────────┤
│   ✗  swipe to cancel        ✓    │
│        hold to confirm           │
└──────────────────────────────────┘
```

Interaction matches `swap_screen` — long-press front button (or hold tap on the ✓ side) to confirm; release / swipe / 30 s timeout cancels. Reusing the swap_screen interaction pattern keeps muscle memory consistent.

### Threading

The caller (`social_x_post`) runs on the speech_task. It pushes args + a semaphore via `lv_async_call`, blocks on the semaphore. The modal lives on the LVGL thread, signals the semaphore + closes itself when resolved.

---

## LLM tool integration

Add `post_to_x` to the tool registry in `ai.c`:

```jsonc
{
  "name": "post_to_x",
  "description": "Drafts a tweet for the user's review. Does NOT post immediately — the user must approve on the device screen.",
  "parameters": {
    "type": "object",
    "properties": {
      "text": { "type": "string", "description": "The post text. ≤ 280 chars." }
    },
    "required": ["text"]
  }
}
```

### Handler behavior

```c
// In ai.c, when the LLM emits a post_to_x tool call:
//   1. Validate text (≤ 280 chars, non-empty)
//   2. Call social_x_post(text), which:
//        a. Opens the approval modal (blocks)
//        b. On CONFIRM: hits api.x.com/2/tweets, returns true/false + reason
//        c. On any CANCEL: returns false with reason="user_cancelled"
//   3. Return the result back to the LLM as the tool result, e.g.:
//        "Posted. URL: https://x.com/.../status/..."
//        "Cancelled by user."
//        "Error: rate_limited / token_expired / network"
//   4. LLM continues the conversation acknowledging the outcome.
```

The LLM sees only the high-level result. It does NOT post directly; the tool gates everything through the modal.

### Voice flow end-to-end

1. User holds PTT, speaks: "post about how the build went today"
2. STT transcript → LLM
3. LLM crafts a draft + emits `post_to_x` tool call
4. `social_x_post()` opens approval modal (LCD)
5. TTS streams: "I drafted a post — approve on screen?"
6. User taps ✓ (long-press) → modal returns CONFIRM
7. `social_x_post()` calls `https://api.x.com/2/tweets` with `Authorization: Bearer <access_token>` and JSON body `{"text": "..."}`
8. On 201: returns the tweet URL to the LLM
9. LLM's reply ("Posted. https://x.com/...") gets TTS'd; user hears it

### Out of scope for v1
- Mid-conversation autonomous posting (LLM only drafts when voice intent is clearly a post request)
- Replying to threads
- Scheduling
- Media attachments
- Multi-account on one device
- Quote-tweets

---

## Storage

NVS namespace `daemon` (existing):

| Key | Type | Notes |
|---|---|---|
| `x_access_tok` | str | OAuth 2.0 access token (~150 chars typical) |
| `x_refresh_tok` | str | OAuth 2.0 refresh token (~150 chars typical) |
| `x_token_exp` | u32 | Unix timestamp when access token expires |
| `x_handle` | str | The user's @handle, for display only |

NVS key length cap is 15 chars — names are pre-truncated.

Encryption: same level as the existing Solana keypair. Currently plaintext per the deliberate "skip eFuse burns in Phase 2b" decision. When NVS encryption gets enabled later, both the wallet key and X tokens are protected together.

---

## Web UI changes

### New SOCIALS tab

In `src/html/index.html`:

```html
<button class="tab" data-tab="socials">SOCIALS</button>
...
<div class="panel" id="panel-socials">
  <div class="card">
    <h3>X (TWITTER)</h3>
    <div id="x-status">
      <!-- one of three states -->
    </div>
  </div>
</div>
```

Three rendered states (decided by `GET /social/x/state`):

**Disconnected:**
```html
<p class="hint">Connect your X account so Daemon can post drafts you approve on-device.</p>
<button class="btn" id="x-connect">Connect X</button>
```

**Pairing in progress (verifier still in RAM):**
```html
<p class="hint">Authorize on x.com, then paste the code below.</p>
<input id="x-code" placeholder="Paste code from x.com..." />
<button class="btn" id="x-finish">Finish pairing</button>
```

**Connected:**
```html
<p>Connected as <strong>@<span id="x-handle"></span></strong></p>
<button class="btn ghost" id="x-disconnect">Disconnect</button>
```

### New HTTP routes (in `src/server.c`)

| Method | Path | Body | Purpose |
|---|---|---|---|
| GET | `/social/x/state` | – | Returns `{ "connected": bool, "handle"?: str, "pairing"?: bool }` |
| POST | `/social/x/begin` | – | Generates verifier+challenge, stores verifier, returns `{ "auth_url": "..." }` for the web UI to open in a new tab |
| POST | `/social/x/finish` | `{ "code": "..." }` | Exchanges code+verifier for tokens, persists, returns `{ "ok": bool, "handle"?: str, "error"?: str }` |
| POST | `/social/x/disconnect` | – | Revokes + clears tokens |

All return `Content-Type: application/json`, follow the existing `send_json()` helper pattern in `server.c`.

---

## x402-bundle changes (separate repo)

Add a single static route: `/x-callback`. The page receives `?code=<auth_code>&state=<pair_id>` from X's redirect and renders:

```
┌────────────────────────────────────┐
│ Daemon — X authorization           │
│                                    │
│ Your code:                         │
│                                    │
│  ┌────────────────────────────┐    │
│  │ AAA-Bbb-CCC-Ddd-EEE-Fff... │    │ <- monospace, large
│  └────────────────────────────┘    │
│                                    │
│  [ Copy code ]                     │
│                                    │
│ Paste this into your Daemon's      │
│ web interface to finish pairing.   │
└────────────────────────────────────┘
```

Stateless. ~30 lines of HTML. No server-side handling — the page reads the URL query string in client JS. Anyone can fork and self-host the bounce page; the OAuth security comes from PKCE on the device side.

The redirect URL registered with X must match exactly. Pin it as `https://daemon-x402s-seven.vercel.app/x-callback` (the existing x402-bundle Vercel deployment), and surface it as a single `#define DAEMON_X_REDIRECT_URI` in firmware so future changes are one-line edits in `social_x.c`. The `client_id` is similarly a `#define DAEMON_X_CLIENT_ID` set from `secrets.h` (or compile-time injection) — same pattern as existing x402 / ElevenLabs credentials.

---

## Files touched

**New:**
- `src/social_x.c` / `src/social_x.h` — pairing flow, token store, posting
- `src/x_post_screen.c` / `src/x_post_screen.h` — approval modal
- `docs/superpowers/specs/2026-04-30-x-posting-design.md` — this file
- *(in x402-bundle repo)* a single static page at `/x-callback`

**Modified:**
- `src/ai.c` — register `post_to_x` tool, route handler to `social_x_post()`
- `src/devcfg.c` / `src/devcfg.h` — getters/setters for the four new NVS keys
- `src/server.c` — four new routes
- `src/html/index.html` — SOCIALS tab + JS
- `src/CMakeLists.txt` — add the two new `.c` files

**Untouched:**
- `src/creature_screen.c` — modal lives in its own screen file
- The Solana wallet path
- The existing chat / LLM / TTS pipeline (only adds one tool)

---

## Failure modes and handling

| Condition | Behavior |
|---|---|
| Token expired (401), refresh succeeds | Retry post once, transparent to user |
| Token expired, refresh fails | Clear tokens, modal closes with error, TTS: "Need to reconnect X" |
| Network down during post | Modal closes with error, TTS: "Couldn't reach X" |
| X rate-limited (429) | Modal closes, TTS: "X is rate-limiting" |
| Draft > 280 chars | Tool handler trims with ellipsis or rejects with "too long"; LLM retries |
| User holds approve, releases mid-confirm | CANCEL_RELEASE — modal closes, no post, TTS: "Cancelled" |
| User taps swipe-to-cancel | CANCEL_SWIPE |
| 30 s elapses with modal idle | CANCEL_TIMEOUT — modal closes |
| Mic input mis-recognized as post intent | LLM drafts something nonsense, user rejects; one wasted draft, no real cost |

---

## Security notes

- **No client secret in firmware.** PKCE eliminates the need for one. Only the public `client_id` is shipped, which is by design public.
- **Verifier never leaves the device.** Generated fresh per pairing, kept in RAM only during pairing.
- **Tokens stored in NVS.** Same threat-model envelope as the wallet key. NVS encryption (eFuse-keyed) protects both when enabled.
- **Bounce page is dumb.** Static HTML. No server logs, no DB, no per-user data.
- **Compromise scope.** A stolen access token only allows posting (and other granted scopes) on the user's account, not transferring funds. Worst case: reconnect on the web UI to revoke + rotate.
- **`offline.access` scope** required for refresh tokens — explicitly request it.

---

## Open questions

None — all design points were resolved during brainstorming:
- Voice-triggered, not button-triggered ✓
- LLM drafts the post, user approves on device ✓
- On-device tokens with PKCE (option B) ✓
- x402-bundle hosts the bounce page ✓
- Pattern mirrors `swap` subsystem ✓
- Text-only for v1 ✓

---

## Implementation order (informational — full plan via writing-plans)

1. **Bounce page** in x402-bundle (smallest piece, no firmware needed; lets us register the redirect URL with X early).
2. **`social_x.c`** — pairing + token storage + POST `/2/tweets`. Without modal yet — direct post for testing.
3. **`x_post_screen.c`** — approval modal (mirror swap_screen).
4. **Wire `social_x_post()` to use the modal.**
5. **`ai.c`** — register `post_to_x` tool.
6. **`server.c` routes** + **HTML SOCIALS tab**.
7. End-to-end voice → draft → approve → post flow.
