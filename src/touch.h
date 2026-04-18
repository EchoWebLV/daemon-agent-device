// ============================================================================
//  Thin CST328 wrapper that produces high-level swipe events. We keep it
//  independent of the screens so each screen can just call touchPollSwipe()
//  from its tick and not worry about I2C timing.
// ============================================================================
#pragma once
#include <Arduino.h>

enum SwipeDir : int8_t {
  SWIPE_NONE  = 0,
  SWIPE_LEFT  = -1,
  SWIPE_RIGHT = +1,
};

bool      touchBegin();
SwipeDir  touchPollSwipe();
