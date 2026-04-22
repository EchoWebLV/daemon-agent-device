# Device wallet — bottom-sheet redesign

## Goal

Replace the current on-device wallet screen's side-swipe transition with
a slide-up-from-bottom reveal, add an explicit X close button, add a QR
button that opens a full-screen QR code of the wallet address, and
tighten the token list so SOL / USDC / SPL tokens read clearly at
240×320.

## Current behavior (what we're changing)

- Wallet is a standalone full-screen (`src/walletscreen.cpp`).
- Opened by swipe LEFT from the creature; closed by swipe RIGHT.
- Snap transition — no animation.
- Status bar shows `WALLET` label and a SOL price ticker.
- Token list truncates with `"..."` when it overflows the visible area.
- No QR code. No close button. Address is text-only.

## New behavior

### Gestures

| Action | Before | After |
|---|---|---|
| Open wallet | `SWIPE_LEFT` from creature | `SWIPE_UP` from creature |
| Close wallet | `SWIPE_RIGHT` from wallet | tap the **X** button (top-right) |
| Open settings | `SWIPE_DOWN` from top | unchanged |

`SWIPE_UP` is not currently mapped on the creature screen, so there's no
collision. `SWIPE_UP` on the settings screen (closes settings) is
unchanged because that handler runs inside the settings-screen branch
of `main.cpp`.

### Open animation

On entry, the wallet is rendered once into a 240×320 sprite in PSRAM.
The main loop then blits the sprite upward from `y=320` down to `y=0`
in ~14 frames of 16 ms each (~220 ms total) using `tft.pushImage`.

Falls back to the current snap redraw if the sprite allocation fails
(e.g. PSRAM exhausted).

### Top bar

```
+------------------------------------------+
| [QR]  WALLET             $86.22   [X]    |
+------------------------------------------+
```

- **QR button** — 24×24 tap rect at `(6, 2)`. Drawn as three
  position-detection-marker squares (7×7 outer, 5×5 inner, 3×3 core)
  in the Daemon accent color. Tap opens the QR overlay.
- **WALLET** — label, accent color, unchanged position.
- **Price ticker** — SOL/USD ticker, unchanged.
- **X button** — 24×24 tap rect at `(SCR_W - 30, 2)`. Drawn as two
  crossed lines (3 px stroke) in the dim text color. Tap returns to
  the creature.

### Main content

Same structure as today:
- Truncated address (unchanged).
- Big SOL balance + USD value (unchanged).
- "HOLDINGS" divider.
- Token list: **USDC first** (detected by mint:
  `EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v`), then all other
  SPL tokens in the current order. Each row gains an 8×8 colored
  square before the symbol; color is derived from the first three
  bytes of the base58-decoded mint, cast to RGB565, so the same mint
  always gets the same color.
- Overflow still truncates with `"..."`. True scroll is out of scope
  (touch driver doesn't emit continuous drag events).

### QR overlay

When the QR button is tapped:
- Black overlay covers the entire wallet screen.
- A QR code of the full base58 public key is rendered using the
  `ricmoo/QRCode` Arduino library (QR version 3, 29×29 modules, ECC
  level L — plenty for a 44-char address).
- Rendered at 6 px per module → 174×174 canvas, centered horizontally,
  top at `y=70`.
- The full address is drawn under the QR in two monospace lines of 22
  chars each.
- Footer: "tap to close" in dim text.
- Any tap dismisses the overlay and returns to the wallet screen.

The overlay is a transient state inside `walletscreen.cpp` — not a
separate `Screen` enum value. It's toggled by a static bool and
redraws happen within `walletScreenDraw()` / `walletScreenTick()`.

## Files touched

| File | Change |
|---|---|
| `src/walletscreen.cpp` | new top-bar (QR + X), slide-in sprite render, QR overlay state + render, USDC-first token order, colored square per row, tap hit-testing for QR/X, overlay dismiss |
| `src/walletscreen.h` | new declarations: `walletScreenHandleTap(int16_t, int16_t)`, `walletScreenConsumeClose()`, `walletScreenOnEnter()` (kicks off the slide animation) |
| `src/main.cpp` | swap `SWIPE_LEFT` → `SWIPE_UP` for wallet open; remove `SWIPE_RIGHT` close; wire `walletScreenConsumeClose()` check like settings has; forward tap coordinates to `walletScreenHandleTap` when on the wallet screen |
| `src/touch.h/.cpp` | expose tap coordinates if not already. (current touch driver emits swipe directions + taps — verify before coding) |
| `platformio.ini` | add `ricmoo/QRCode` to `lib_deps` |

## Non-goals

- No drag-scroll for the token list (touch driver limitation). Overflow
  still truncates.
- No send/swap/refresh action buttons.
- No token logos (would need an image fetch + cache). Colored squares
  stand in.
- No swipe-down-to-close gesture (the X button is the only close).

## Testing

Manual:
1. Boot device, verify creature is shown.
2. Swipe UP → wallet slides up, top bar shows QR and X.
3. Tap X → wallet closes, creature returns.
4. Swipe UP again → wallet re-opens with animation.
5. Tap QR → QR overlay shown, address printed below.
6. Tap anywhere → overlay dismisses, wallet visible again.
7. Verify USDC row is first in HOLDINGS when wallet holds USDC.
8. Verify colored squares are consistent across re-draws.
9. Swipe DOWN from wallet → settings opens (existing behavior,
   confirm not broken).

## Risks

- `ricmoo/QRCode` RAM footprint — should be OK (version 3 code is
  ~150 bytes). Confirm at build time.
- PSRAM sprite for slide animation (~150 KB). Need to check free
  PSRAM budget; the Audio library also allocates from PSRAM. If tight,
  fall back to row-by-row draw.
- Tap hit-testing depends on the current touch driver exposing tap
  coordinates; if it only emits swipe directions, the touch driver
  needs a small extension first.
