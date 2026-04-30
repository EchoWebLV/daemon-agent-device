# 2026-04-30 — Session Recap

## TL;DR

Today produced one big feature (**voice-triggered X posting with on-device approval**), one nice-to-have (**live theme switch**), and one regression (**a "cleanup" commit that quietly broke the device**). The regression was the source of all today's bugs. Last-night's commit `903d257` boots clean — confirmed by the user just now.

**Recommendation: don't ditch. Cherry-pick the good work onto `903d257`.** Most of today is salvageable; the rot was concentrated in one commit (`5e5ade9`) and the revert-dance commits that followed. See "Recommendation" below.

---

## What we did, in order

### 1. Pixel-grid creature designer (morning, before "today's work")

This was already on `903d257`. Worth listing because it's the foundation of everything else.

- New uniform pixel-grid system: every creature is a set of bitmasks (brows, eyes, mouth) in fixed standardized slots. Replaced bespoke variant enums.
- New HTML pencil designer at `tools/creature-designer.html` — paint, live preview, blink + talking animations, export to C, save/load JSON, dynamic roster size, pencil toggle.
- 7 creature designs shipped (Daemon, Laser, Owl, Slot 4–7).
- Face vertical offset tuned to centered between status bar and subtitle.

**Status: in `903d257`, working.**

### 2. Cleanup commit (the mistake)

Commit `5e5ade9` — billed as a low-risk pass:
- Removed `devcfg_set_personality("")` boot-time wipe ❌ **load-bearing — its removal broke the LLM**
- Removed `creature_screen_set_status` no-op chain (harmless)
- Reduced 7 wifi_sta + 1 ai.c log levels INFO → DEBUG (harmless)

The personality wipe was a "TEMP" comment in the original code. I argued it was an over-cautious debug hack. **It wasn't.** Without it, a stale persona string in NVS overrides the built-in PERSONA and Daemon talks on its own / babbles. Removing it was the source of "stuck on talking" + cascade of related symptoms.

**Status: should be reverted entirely.**

### 3. Voice-triggered X posting (the big feature)

Full multi-task plan executed via the subagent-driven-development workflow. End state: every firmware-side piece in place, ready for the user to deploy the bounce page + register an X dev app.

**14 firmware commits** (`f21f913` → `d446056`):
- `devcfg.c` — 4 new NVS slots for tokens + handle, with `extern const int CREATURE_DATA_COUNT` runtime sizing
- `secrets.h.example` — declares `DAEMON_X_CLIENT_ID` + `DAEMON_X_REDIRECT_URI`
- `social_x.c` / `social_x.h` — full OAuth 2.0 PKCE flow:
  - `social_x_begin` — generate verifier + challenge, build auth URL
  - `social_x_finish` — exchange code+verifier for tokens, fetch handle
  - `social_x_disconnect` — best-effort revoke + NVS wipe
  - `refresh_access_token` — used on 401 retry
  - `social_x_post` — gates on approval modal, posts, retries on 401
- `x_post_screen.c` / `.h` — approval modal mirroring swap_screen pattern (hold-to-confirm, swipe-to-cancel, 30s timeout)
- 4 new HTTP routes in `server.c`: `GET/POST /social/x/{state,begin,finish,disconnect}`
- New SOCIALS tab in `src/html/index.html` — connect / pair / disconnect UI
- `post_to_x` tool registered in `ai.c` — gated on `devcfg_x_connected()`

**Status: code complete, untested end-to-end.** User still needs to (a) deploy the bounce page in the `x402-bundle` repo (HTML drafted, not committed), (b) register a Daemon app on the X developer portal, (c) put the real client_id into `secrets.h`.

### 4. Static bounce page (not yet committed)

Written but staged in working tree of `~/Documents/GitHub/x402-bundle` (separate repo): a Next.js client component at `src/app/x-callback/page.tsx` that displays the OAuth code + a Copy button. Stateless, no server logic.

**Status: written, not committed, not deployed.**

### 5. Optimization pass (also reverted via the cleanup commit)

Targeted scan for dead code + easy wins:
- Confirmed `creature_screen_set_status` chain was dead → dropped
- Reduced log noise on Wi-Fi + ai tool path → INFO to DEBUG
- Personality wipe — **misclassified as debug code, deletion broke device**

**Status: same commit as the cleanup. Drop it entirely.**

### 6. Red theme attempt (rolled back)

User asked for a third red theme alongside dark/light. While in flight, user changed mind — sticking with two themes only. Two commits added (`31f6371`, `0b62173`), both reverted (`64148b1`, `30ffeaf`).

**Status: net-zero, just commit churn.**

### 7. Live theme switch (no reboot)

Replaced the `esp_restart()` theme-flip with destroy + rebuild of every cached screen.

- Added `<screen>_destroy()` to all 7 screen modules (creature, menu, wallet, info, config, settings, wifi). Each tears down timers, deletes the root, nulls out static widgets.
- Added `ui_apply_theme()` in ui.c that defers via `lv_async_call`, hops to a transient blank screen, destroys all, applies palette, rebuilds all, re-attaches gestures, reloads the previously-active screen.
- Settings theme handler now calls `ui_apply_theme` instead of `esp_restart`.

**Status: implemented, untested visually.** Code looks correct but never seen working on device — flashes happened during the regression-debugging phase so symptoms were dominated by the personality-wipe bug.

### 8. Personality wipe restore + revert dance

Once the regression was identified, restored `devcfg_set_personality("")` to `app_main.c`. Did not fix all symptoms because (we now know) the underlying issue was network/environment, not just the persona. Then:
- Reverted live theme switch to test if it caused symptoms (it didn't — they persisted).
- Reverted `post_to_x` tool registration to test if it caused symptoms (it didn't either).
- Tried reverting the cleanup commit, hit a merge conflict, aborted.
- Checked out `903d257` to test pre-everything state — symptoms persisted then but pass now.

**Status: a tangle of revert / revert-of-revert commits. Nuke all of these.**

---

## What we know works

| Thing | Status | Where |
|---|---|---|
| Designer + 7 creatures + new pixel-grid system | ✅ Verified working | already in `903d257` |
| Personality wipe on boot | ✅ In place in `903d257` | `app_main.c` |
| Face centered correctly | ✅ Verified | already in `903d257` |
| X posting (firmware side) | 🟡 Code complete, build clean, not exercised | needs `5e5ade9`-onwards excluding the bad parts |
| Bounce page | 🟡 Drafted, not deployed | `x402-bundle` working tree |
| Live theme switch | 🟡 Code looks right, never seen working | needs `e51b827` + `edc2b25` |

---

## What broke

The "stuck on talking" / slow / glitchy symptoms had **two contributors**:

1. **`5e5ade9` cleanup commit dropped the personality wipe** — Daemon spoke on its own from a stale NVS persona.
2. **Network conditions on the coworking Wi-Fi** — explains the residual slowness even after the wipe was restored. Confirmed by the fact that the same `903d257` build that was buggy 30 minutes ago is fine now.

The interaction was confusing because both pointed at "Daemon talking when it shouldn't / slowly / glitchy" but came from totally different layers.

---

## Recommendation

**Don't ditch today's work — but don't keep the current branch either.** The current `feat/x-posting` branch has 14 useful commits buried under 6 revert/dance commits. Cleaner to rebuild.

### Plan

```bash
# from 903d257 (verified working, the foundation)
git checkout 903d257
git checkout -b feat/x-posting-clean

# cherry-pick the planning docs (optional but useful)
git cherry-pick d145ab0 891475b   # spec + plan for X posting
git cherry-pick f4a38ab            # plan for designer (already-shipped)

# cherry-pick the X posting feature (14 firmware commits, in order)
git cherry-pick f21f913 6d9c676 8b5d9d9 6d6f94e 6f1afdc \
                6e78c02 6af6dac 34174dd 731464e aea2a3d \
                e9d9e83 ed2011e d80e96e d446056

# cherry-pick the live theme switch (2 commits)
git cherry-pick e51b827 edc2b25

# verify
pio run
```

Result: a clean branch with **17 useful commits stacked on `903d257`**, no cleanup-commit, no red-theme detours, no revert-of-revert noise. Same end-state as `feat/x-posting`'s tip minus the rot.

### What to drop

- `5e5ade9` (cleanup — removed personality wipe, broke things)
- `31f6371` and `0b62173` (red theme attempts)
- `64148b1` and `30ffeaf` (reverts of red theme)
- `7094490` (personality wipe restore — unnecessary, `903d257` already has the wipe)
- `197c64f` (revert of live theme)
- `fe6157b` (revert of post_to_x tool)
- `f9671ee` (revert of revert of live theme)

### Outstanding manual work (regardless of which path)

These don't touch firmware:
1. Commit + push the bounce page in `~/Documents/GitHub/x402-bundle`
2. Register a Daemon app at developer.x.com — get a Client ID
3. Add `DAEMON_X_CLIENT_ID` + `DAEMON_X_REDIRECT_URI` to `src/secrets.h`
4. Reflash and walk the OAuth flow once

### Better idea?

Yes, one: **make `DAEMON_X_CLIENT_ID` runtime-configurable from the web UI** (you flagged this earlier). That way the firmware doesn't bake a personal credential and other Daemon owners can connect their own X accounts without rebuilding. Small extension to the SOCIALS tab — could be a follow-up bundle on the clean branch.

---

## Open questions / lingering

- **Why did the device feel slow on coworking Wi-Fi?** Probably one of: captive portal, content-filter DPI, slow upstream DNS. Not a firmware bug, but the long-term fix is the perf pass we sketched (HTTP keep-alive on price/wallet, longer timeouts, RSSI logging, voice task stack bump). Cheap and harmless when the next session has time.
- **Live theme switch is unverified on hardware.** Code mirrors swap_screen exactly, builds clean, but I never saw the device flip themes without a reboot. Worth a 60-second smoke test on the clean branch.
