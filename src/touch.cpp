#include "touch.h"
#include <Wire.h>
#include <CSE_CST328.h>

// Pins per the Waveshare 2.8" wiki
static constexpr uint8_t  PIN_SDA   = 1;
static constexpr uint8_t  PIN_SCL   = 3;
static constexpr int8_t   PIN_INT   = 4;
static constexpr int8_t   PIN_RST   = 2;
static constexpr uint32_t I2C_HZ    = 400000;
static constexpr int16_t  SCR_W     = 240;
static constexpr int16_t  SCR_H     = 320;

// Swipe detection parameters. Kept conservative so vertical scrolls and
// taps don't accidentally navigate between screens.
static constexpr int16_t  MIN_DX        = 70;    // horizontal travel
static constexpr int16_t  MAX_DY        = 60;    // vertical slop
static constexpr uint32_t MAX_DURATION  = 900;   // ms; longer = probably not a swipe

static CSE_CST328 touch(SCR_W, SCR_H, &Wire, PIN_RST, PIN_INT);
static bool       s_ok = false;

// Gesture state
static bool       s_tracking = false;
static int16_t    s_startX = 0, s_startY = 0;
static int16_t    s_lastX  = 0, s_lastY  = 0;
static uint32_t   s_startMs = 0;

bool touchBegin() {
  Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
  s_ok = touch.begin();
  if (s_ok) touch.setRotation(0);
  Serial.printf("touch: CST328 %s\n", s_ok ? "OK" : "FAILED");
  return s_ok;
}

SwipeDir touchPollSwipe() {
  if (!s_ok) return SWIPE_NONE;

  uint8_t n = touch.getTouches();
  if (n > 0) {
    CSE_TouchPoint p = touch.getPoint(0);
    if (!s_tracking) {
      s_tracking = true;
      s_startX   = p.x;
      s_startY   = p.y;
      s_startMs  = millis();
    }
    s_lastX = p.x;
    s_lastY = p.y;
    return SWIPE_NONE;
  }

  // Finger up.
  if (s_tracking) {
    s_tracking = false;
    int16_t dx = s_lastX - s_startX;
    int16_t dy = s_lastY - s_startY;
    uint32_t dt = millis() - s_startMs;
    if (dt <= MAX_DURATION && abs(dy) <= MAX_DY) {
      if (dx <= -MIN_DX) return SWIPE_LEFT;
      if (dx >=  MIN_DX) return SWIPE_RIGHT;
    }
  }
  return SWIPE_NONE;
}
