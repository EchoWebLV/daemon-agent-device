#include "statusicons.h"
#include "devcfg.h"

// Icon footprint. 14 wide × 12 tall fits comfortably inside the 16-px
// tall status bar with a one-pixel bleed on top and bottom.
static constexpr int ICON_W = 14;
static constexpr int ICON_H = 12;
static constexpr int GAP    = 6;

// Memory glyph — three stacked rounded bars that read as "data rows /
// stored records". Small enough to live next to the heart without
// crowding the status bar.
static void drawMemoryIcon(TFT_eSPI *tft, int x, int y, uint16_t color) {
  for (int row = 0; row < 3; ++row) {
    int yy = y + row * 4;
    tft->drawRoundRect(x, yy, ICON_W, 3, 1, color);
  }
}

// Heartbeat glyph — classic filled heart (two circles + triangle).
static void drawHeartIcon(TFT_eSPI *tft, int x, int y, uint16_t color) {
  tft->fillCircle(x + 4,  y + 4, 3, color);
  tft->fillCircle(x + 10, y + 4, 3, color);
  tft->fillTriangle(x + 1, y + 5, x + ICON_W - 1, y + 5,
                    x + ICON_W / 2, y + ICON_H - 1, color);
}

// Cached state so screens can short-circuit work when nothing changed.
static bool s_lastMem = false;
static bool s_lastHb  = false;
static bool s_valid   = false;

void statusIconsDraw(TFT_eSPI *tft, int centerX, int centerY,
                     uint16_t color, uint16_t bgColor) {
  bool mem = devcfgMemoryEnabled();
  bool hb  = devcfgHeartbeatEnabled();

  int totalW = (mem && hb) ? (ICON_W * 2 + GAP)
             : (mem || hb) ? ICON_W
             : 0;

  // Always clear a generous band in the center so any previously drawn
  // icons are wiped when the toggles flip off.
  int clearW = ICON_W * 2 + GAP + 4;
  tft->fillRect(centerX - clearW / 2, centerY - ICON_H / 2 - 1,
                clearW, ICON_H + 2, bgColor);

  if (totalW > 0) {
    int x = centerX - totalW / 2;
    int y = centerY - ICON_H / 2;
    if (mem) {
      drawMemoryIcon(tft, x, y, color);
      x += ICON_W + GAP;
    }
    if (hb) {
      drawHeartIcon(tft, x, y, color);
    }
  }

  s_lastMem = mem;
  s_lastHb  = hb;
  s_valid   = true;
}

bool statusIconsNeedRedraw() {
  if (!s_valid) return true;
  return devcfgMemoryEnabled()   != s_lastMem
      || devcfgHeartbeatEnabled() != s_lastHb;
}

void statusIconsResetCache() { s_valid = false; }
