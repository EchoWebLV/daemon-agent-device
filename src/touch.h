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
