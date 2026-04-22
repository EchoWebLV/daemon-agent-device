// ============================================================================
//  Tiny screen-effects helpers shared across the full-screen panels, so the
//  wallet / menu / info / settings / wifi screens can animate and draw
//  common chrome the same way without duplicating code.
// ============================================================================
#pragma once
#include <TFT_eSPI.h>

// Dimensions of the ESP32-S3-Touch-LCD-2.8 panel (portrait).
static constexpr int16_t SCREENFX_W = 240;
static constexpr int16_t SCREENFX_H = 320;

// ---------------------------------------------------------------------------
// Unified UI palette. Every panel should pull its colors from here so the
// device feels like one OS instead of a pile of scratch paint jobs.
// ---------------------------------------------------------------------------

// Background — a deep indigo base, not pure black. Adds perceived depth.
static constexpr uint16_t UI_C_BG          = 0x0861;
static constexpr uint16_t UI_C_BG_DEEP     = 0x0020;   // near-black, for shadows

// Card surfaces.
static constexpr uint16_t UI_C_CARD        = 0x18E4;   // body fill
static constexpr uint16_t UI_C_CARD_HI     = 0x31A6;   // border highlight
static constexpr uint16_t UI_C_CARD_INSET  = 0x1082;   // inset field inside card

// Accents.
static constexpr uint16_t UI_C_ACCENT      = 0x0AFF;   // Daemon cyan-blue
static constexpr uint16_t UI_C_ACCENT_HI   = 0x07FF;   // bright cyan rim
static constexpr uint16_t UI_C_ACCENT_DIM  = 0x031F;   // dim cyan, for outlines
static constexpr uint16_t UI_C_WARN        = 0xFBE0;   // amber — info tile accent
static constexpr uint16_t UI_C_WARN_DIM    = 0x7AC0;
static constexpr uint16_t UI_C_GREEN       = 0x3EE6;   // soft green

// Text.
static constexpr uint16_t UI_C_TEXT        = 0xFFFF;
static constexpr uint16_t UI_C_TEXT_DIM    = 0xBDF7;
static constexpr uint16_t UI_C_TEXT_DIMMER = 0x7BEF;
static constexpr uint16_t UI_C_DIM         = 0x4A49;

// ---------------------------------------------------------------------------
// Animations + widgets.
// ---------------------------------------------------------------------------

// Animate a screen onto the display by painting it into an off-screen
// PSRAM sprite and blitting the sprite from `fromY` to 0 over ~220 ms
// with a quadratic ease-out.
//   fromY = +SCREENFX_H  → slide UP from the bottom edge (wallet / menu)
//   fromY = -SCREENFX_H  → slide DOWN from the top edge (settings)
// The caller's `paint` callback is invoked once with the sprite
// (wrapped in a TFT_eSPI*) as the render target; existing painters that
// use a module-local `s_tft` can be reused by re-pointing it for the
// duration of the call. Returns false if the sprite allocation failed,
// in which case the caller should paint straight to the display.
bool screenfxSlideIn(TFT_eSPI *tft,
                     int16_t   fromY,
                     void    (*paint)(TFT_eSPI *));

// Consistent "x" close button used by every full-screen panel's top bar.
// 28×24 rounded-rect frame with two 2-px diagonal strokes inside.
void screenfxDrawXButton(TFT_eSPI *tft,
                         int16_t   x,
                         int16_t   y,
                         uint16_t  fg,
                         uint16_t  bg);

// Width/height of the X button drawn by screenfxDrawXButton. Exposed so
// callers can keep tap hit-rects in sync with the visual.
static constexpr int16_t SCREENFX_X_BTN_W = 28;
static constexpr int16_t SCREENFX_X_BTN_H = 24;

// Draw a card-shaped surface: drop-shadow, body, border, and a vertical
// accent stripe on the left edge. Used by the menu tiles and the wallet
// section groupings so every "card" on-device is physically the same
// card. Colors come from the UI_C_* palette above.
void screenfxDrawCard(TFT_eSPI *tft,
                      int16_t   x,
                      int16_t   y,
                      int16_t   w,
                      int16_t   h,
                      uint16_t  accent);

// A thin 1-px horizontal divider rule. Draws in UI_C_CARD_HI by default.
void screenfxDivider(TFT_eSPI *tft, int16_t y);
