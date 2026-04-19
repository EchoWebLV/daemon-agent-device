// ============================================================================
//  Sliding screen transitions.
//
//  Animates the swap between two full-screen views (creature, menu,
//  wallet, settings, info, X) as a vertical push: both screens move
//  together so the incoming one slides into view while the outgoing one
//  slides off in the same direction.
//
//  Implementation uses two 240×320 16-bpp sprites in PSRAM (~150 KB
//  each). Each screen module exposes a `*ScreenDrawTo(TFT_eSprite *)`
//  hook that re-runs its full paint into the supplied sprite; the
//  transition module pre-renders both screens into the two sprites
//  then animates per-frame slices of each onto the live TFT.
//
//  The animation is blocking — touchPoll / creatureTick / serverLoop
//  pause for ~210 ms. At 60+ fps that's ~12-14 frames, which feels
//  iOS-snappy without trapping the user mid-gesture.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

enum SlideDir : uint8_t {
  SLIDE_UP   = 0,   // incoming rises from bottom; outgoing leaves at top
  SLIDE_DOWN = 1,   // incoming drops from top;     outgoing leaves at bottom
};

// One-time setup. Allocates the two PSRAM transition sprites. Returns
// false if PSRAM allocation fails — in that case transitionRunSlide()
// gracefully degrades to an instant snap (no animation).
bool transitionBegin(TFT_eSPI *tft);

// Renders incoming + outgoing into the internal sprites via the
// supplied draw callbacks, then runs the slide animation in the
// requested direction. Blocks until the animation finishes.
//
// Either drawIncoming or drawOutgoing may be nullptr — in that case the
// missing side is treated as a fully-black screen, which makes for a
// reasonable visual fallback (e.g. for screens that don't yet have a
// DrawTo wrapper).
using TransitionDraw = void (*)(TFT_eSprite *target);
void transitionRunSlide(SlideDir dir,
                        TransitionDraw drawIncoming,
                        TransitionDraw drawOutgoing);
