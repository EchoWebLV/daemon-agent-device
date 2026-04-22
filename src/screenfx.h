// ============================================================================
//  Tiny screen-effects helpers shared across the full-screen panels, so the
//  wallet / settings / wifi screens can animate and draw common chrome the
//  same way without duplicating code.
// ============================================================================
#pragma once
#include <TFT_eSPI.h>

// Dimensions of the ESP32-S3-Touch-LCD-2.8 panel (portrait).
static constexpr int16_t SCREENFX_W = 240;
static constexpr int16_t SCREENFX_H = 320;

// Animate a screen onto the display by painting it into an off-screen
// PSRAM sprite and blitting the sprite from `fromY` to 0 over ~220 ms with
// a quadratic ease-out. Typical values:
//   fromY = +SCREENFX_H  → slide UP from the bottom edge (wallet sheet)
//   fromY = -SCREENFX_H  → slide DOWN from the top edge (settings sheet)
// The caller's `paint` callback is invoked once with the sprite (wrapped
// in a TFT_eSPI*) as the render target, so existing painters that use a
// module-local `s_tft` can be reused simply by re-pointing it for the
// duration of the call.
// Returns false if the sprite allocation failed, in which case the caller
// should paint straight to the display as a no-animation fallback.
bool screenfxSlideIn(TFT_eSPI *tft,
                     int16_t   fromY,
                     void    (*paint)(TFT_eSPI *));

// Clean, consistent "x" close button used by every full-screen panel's
// top bar. Drawn as a 28×24 rounded-rect frame with two 2-px diagonal
// strokes inside, both in the provided foreground color.
void screenfxDrawXButton(TFT_eSPI *tft,
                         int16_t   x,
                         int16_t   y,
                         uint16_t  fg,
                         uint16_t  bg);

// Width/height of the X button drawn by screenfxDrawXButton. Exposed so
// callers can keep tap hit-rects in sync with the visual.
static constexpr int16_t SCREENFX_X_BTN_W = 28;
static constexpr int16_t SCREENFX_X_BTN_H = 24;
