// ============================================================================
//  CST328 touchscreen wrapper.
//
//  Emits high-level swipe gestures (left/right/up, plus a "pull-down from
//  the top edge" which opens the settings panel) and exposes a live
//  finger position so widgets like the settings-screen sliders can drag.
// ============================================================================
#pragma once
#include <Arduino.h>

enum SwipeDir : int8_t {
  SWIPE_NONE  = 0,
  SWIPE_LEFT  = -1,   // ← go to wallet (from creature)
  SWIPE_RIGHT = +1,   // → back to creature (from wallet)
  SWIPE_UP    = -2,   // ↑ close settings
  SWIPE_DOWN  = +2,   // ↓ open settings (must start near top edge)
};

bool     touchBegin();

// Call once per frame. Updates internal gesture + position state and
// returns any completed swipe (at finger-up).
SwipeDir touchPoll();

// True while a finger is currently on the glass. Cleared on finger-up.
// Writes the current screen-space position into (x, y).
bool     touchActive(int16_t &x, int16_t &y);

// True for exactly one frame right after the finger first touches down.
// Handy for simple "tap" logic without a full gesture machine.
bool     touchJustPressed(int16_t &x, int16_t &y);

// How long the finger has been continuously on the glass, in
// milliseconds since the initial press. Returns 0 when no finger is
// touching. Designed for "hold to trigger" interactions — e.g. the
// creature zoom-out kicks in after 800 ms of unbroken press.
uint32_t touchPressDurationMs();

// Tell the touch layer to NOT emit a swipe on the next finger-up. Use
// this after consuming a long-press so the lift doesn't also fire
// whatever swipe the accidental drag might have accumulated into.
// The suppression clears itself after the next release is ignored,
// so subsequent gestures behave normally.
void     touchConsumeRelease();
