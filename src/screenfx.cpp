#include "screenfx.h"
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Slide-in animation. A single 240×320 16-bpp sprite is ~150 KB; we pin it
// to PSRAM so the audio decoder's big heap buffer isn't evicted during the
// ~220 ms the animation runs.
// ---------------------------------------------------------------------------
bool screenfxSlideIn(TFT_eSPI *tft,
                     int16_t   fromY,
                     void    (*paint)(TFT_eSPI *)) {
  if (!tft || !paint) return false;

  TFT_eSprite sprite(tft);
  sprite.setColorDepth(16);
  sprite.setAttribute(PSRAM_ENABLE, true);
  if (!sprite.createSprite(SCREENFX_W, SCREENFX_H)) {
    Serial.println("screenfx: sprite alloc failed");
    return false;
  }

  // Let the caller render its finished screen into the sprite. Painters
  // that use a module-local TFT_eSPI pointer just re-point it to &sprite
  // for the duration of this callback.
  paint(&sprite);

  // Clear the real display once; pushSprite will progressively overwrite
  // the rest of the area, and whatever is still uncovered stays black
  // instead of flashing the old screen.
  tft->fillScreen(TFT_BLACK);

  // 14 frames × 15 ms ≈ 210 ms. Quadratic ease-out so the sheet
  // decelerates into place instead of snapping.
  constexpr int FRAMES = 14;
  for (int i = 1; i <= FRAMES; ++i) {
    float   t     = (float)i / (float)FRAMES;
    float   eased = 1.0f - (1.0f - t) * (1.0f - t);
    int16_t y     = (int16_t)(fromY * (1.0f - eased));
    sprite.pushSprite(0, y);
    delay(15);
  }
  sprite.pushSprite(0, 0);
  sprite.deleteSprite();
  return true;
}

// ---------------------------------------------------------------------------
// Unified X close button — same across wallet / settings / wifi so the
// top-right corner of every panel looks identical.
// ---------------------------------------------------------------------------
void screenfxDrawXButton(TFT_eSPI *tft,
                         int16_t   x,
                         int16_t   y,
                         uint16_t  fg,
                         uint16_t  bg) {
  constexpr int16_t W = SCREENFX_X_BTN_W;   // 28
  constexpr int16_t H = SCREENFX_X_BTN_H;   // 24

  tft->fillRect(x, y, W, H, bg);
  tft->drawRoundRect(x, y, W, H, 5, fg);

  // Two 2-px-thick diagonal strokes crossing at the centre.
  const int16_t cx = x + W / 2;
  const int16_t cy = y + H / 2;
  const int16_t r  = 6;
  tft->drawLine(cx - r,     cy - r,     cx + r,     cy + r,     fg);
  tft->drawLine(cx - r + 1, cy - r,     cx + r + 1, cy + r,     fg);
  tft->drawLine(cx - r,     cy + r,     cx + r,     cy - r,     fg);
  tft->drawLine(cx - r + 1, cy + r,     cx + r + 1, cy - r,     fg);
}

// ---------------------------------------------------------------------------
// Card surface — layered fake-depth rectangle. We don't have alpha on this
// TFT pipeline, so the shadow is a second rounded-rect drawn one pixel
// down and to the right in a near-black; the body sits on top, the border
// is a single highlight stroke, and a 4-px-wide accent stripe on the left
// edge anchors the whole thing visually.
//
// Callers pass the accent color, which lets one card be cyan (wallet) and
// another amber (info) without duplicating code.
// ---------------------------------------------------------------------------
void screenfxDrawCard(TFT_eSPI *tft,
                      int16_t   x,
                      int16_t   y,
                      int16_t   w,
                      int16_t   h,
                      uint16_t  accent) {
  // Drop shadow: slightly larger rect behind the body in near-black.
  tft->fillRoundRect(x + 1, y + 3, w, h, 12, UI_C_BG_DEEP);
  // Body fill.
  tft->fillRoundRect(x, y, w, h, 12, UI_C_CARD);
  // Highlight border — one pixel of perceived bevel.
  tft->drawRoundRect(x, y, w, h, 12, UI_C_CARD_HI);
  // Left accent stripe. Tall and thin so it reads as a file-folder tab.
  tft->fillRoundRect(x + 6, y + 10, 4, h - 20, 2, accent);
}

// ---------------------------------------------------------------------------
// 1-px horizontal divider rule in the card-highlight color. Used by info
// and other list-style panels between sections.
// ---------------------------------------------------------------------------
void screenfxDivider(TFT_eSPI *tft, int16_t y) {
  tft->fillRect(0, y, SCREENFX_W, 1, UI_C_CARD_HI);
}
