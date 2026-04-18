#include "touch.h"
#include <Wire.h>
#include <CSE_CST328.h>

// ---- Pin + bus config (Waveshare 2.8") ------------------------------------
static constexpr uint8_t  PIN_SDA   = 1;
static constexpr uint8_t  PIN_SCL   = 3;
static constexpr int8_t   PIN_INT   = 4;
static constexpr int8_t   PIN_RST   = 2;
static constexpr uint32_t I2C_HZ    = 400000;
static constexpr int16_t  SCR_W     = 240;
static constexpr int16_t  SCR_H     = 320;

// ---- Swipe thresholds -----------------------------------------------------
static constexpr int16_t  MIN_H_DX       = 70;    // horizontal swipe travel
static constexpr int16_t  MAX_H_DY       = 60;    // vertical slop for H swipe
static constexpr int16_t  MIN_V_DY       = 70;    // vertical swipe travel
static constexpr int16_t  MAX_V_DX       = 60;    // horizontal slop for V swipe
static constexpr int16_t  TOP_EDGE_Y     = 40;    // "pull-down" must start in top strip
static constexpr uint32_t MAX_DURATION   = 900;

static CSE_CST328 touch(SCR_W, SCR_H, &Wire, PIN_RST, PIN_INT);
static bool       s_ok = false;

// Gesture state
static bool       s_tracking   = false;
static bool       s_justPressed = false;
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

SwipeDir touchPoll() {
  s_justPressed = false;
  if (!s_ok) return SWIPE_NONE;

  uint8_t n = touch.getTouches();
  if (n > 0) {
    CSE_TouchPoint p = touch.getPoint(0);
    if (!s_tracking) {
      s_tracking = true;
      s_justPressed = true;
      s_startX   = p.x;
      s_startY   = p.y;
      s_startMs  = millis();
    }
    s_lastX = p.x;
    s_lastY = p.y;
    return SWIPE_NONE;
  }

  // Finger lifted — evaluate the accumulated motion.
  if (s_tracking) {
    s_tracking = false;
    int16_t dx = s_lastX - s_startX;
    int16_t dy = s_lastY - s_startY;
    uint32_t dt = millis() - s_startMs;
    if (dt > MAX_DURATION) return SWIPE_NONE;

    // Horizontal swipes
    if (abs(dy) <= MAX_H_DY) {
      if (dx <= -MIN_H_DX) return SWIPE_LEFT;
      if (dx >=  MIN_H_DX) return SWIPE_RIGHT;
    }
    // Vertical swipes
    if (abs(dx) <= MAX_V_DX) {
      if (dy <= -MIN_V_DY) return SWIPE_UP;
      if (dy >=  MIN_V_DY && s_startY <= TOP_EDGE_Y) return SWIPE_DOWN;
    }
  }
  return SWIPE_NONE;
}

bool touchActive(int16_t &x, int16_t &y) {
  if (!s_tracking) return false;
  x = s_lastX;
  y = s_lastY;
  return true;
}

bool touchJustPressed(int16_t &x, int16_t &y) {
  if (!s_justPressed) return false;
  x = s_startX;
  y = s_startY;
  return true;
}
