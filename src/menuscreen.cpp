#include "menuscreen.h"
#include "screenfx.h"
#include "price.h"

static constexpr int16_t SCR_W = SCREENFX_W;
static constexpr int16_t SCR_H = SCREENFX_H;

// Top bar geometry — matches wallet / info so the header rail is pixel-
// aligned across every panel.
static constexpr int16_t TOPBAR_Y = 3;
static constexpr int16_t CLOSE_W  = SCREENFX_X_BTN_W;
static constexpr int16_t CLOSE_H  = SCREENFX_X_BTN_H;
static constexpr int16_t CLOSE_X  = SCR_W - CLOSE_W - 6;
static constexpr int16_t CLOSE_Y  = TOPBAR_Y;

// Two full-width tiles stacked vertically, each ~116 px tall.
static constexpr int16_t TILE_X   = 10;
static constexpr int16_t TILE_W   = SCR_W - 2 * TILE_X;
static constexpr int16_t TILE_H   = 116;
static constexpr int16_t TILE1_Y  = 44;
static constexpr int16_t TILE2_Y  = TILE1_Y + TILE_H + 12;   // 172

static TFT_eSPI *s_tft = nullptr;

// Latched intents.
static bool s_wantClose     = false;
static bool s_wantWalletTap = false;
static bool s_wantInfoTap   = false;

// We only tick the status bar (for the SOL price ticker); the tiles are
// static so there's nothing to refresh there, and no flicker from mid-
// tile repaints.
static uint32_t s_lastTickMs = 0;
static String   s_lastPrice;

// ---------------------------------------------------------------------------
// Icons — drawn programmatically so we ship no PNGs. Each icon sits inside
// a 56×56 square whose center is passed in.
// ---------------------------------------------------------------------------

// Wallet icon: a soft "bill fold" silhouette with a dollar mark on the
// card face. Two layered rounded rects fake an inset clasp.
static void drawWalletIcon(int16_t cx, int16_t cy) {
  const int16_t w = 48, h = 36;
  const int16_t x = cx - w / 2;
  const int16_t y = cy - h / 2;

  s_tft->fillRoundRect(x + 1, y + 2, w, h, 7, UI_C_BG_DEEP);   // drop shadow
  s_tft->fillRoundRect(x, y, w, h, 7, UI_C_ACCENT);            // card body
  s_tft->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 5, UI_C_ACCENT_HI);
  s_tft->fillRect(x + 4, y + 8, w - 8, 3, UI_C_BG_DEEP);       // mag-stripe

  s_tft->setFreeFont(&FreeSansBold12pt7b);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(UI_C_TEXT, UI_C_ACCENT);
  s_tft->drawString("$", cx, cy + 5);
  s_tft->setFreeFont(nullptr);
}

// Info icon: a two-ring amber disc with a lowercase "i" glyph.
static void drawInfoIcon(int16_t cx, int16_t cy) {
  const int16_t r = 22;
  s_tft->fillCircle(cx + 1, cy + 2, r, UI_C_BG_DEEP);          // drop shadow
  s_tft->fillCircle(cx, cy, r, UI_C_WARN);                     // disc
  s_tft->drawCircle(cx, cy, r,     UI_C_WARN_DIM);             // outer rim
  s_tft->drawCircle(cx, cy, r - 4, UI_C_WARN_DIM);             // inner rim
  s_tft->fillCircle(cx, cy - 8, 2, UI_C_BG_DEEP);              // "i" dot
  s_tft->fillRect(cx - 2, cy - 3, 4, 12, UI_C_BG_DEEP);        // "i" stem
}

// ---------------------------------------------------------------------------
// Tile painter — icon on the left, a single big label in the middle, and
// a chevron on the right. No subtitle, no caption: the tile is static so
// we paint it once and never touch it again until the next screen enter.
// ---------------------------------------------------------------------------
typedef void (*IconFn)(int16_t, int16_t);

static void paintTile(int16_t y,
                      const char *label,
                      uint16_t    accent,
                      IconFn      iconFn) {
  // Card chrome — shadow + body + highlight border + left accent stripe.
  screenfxDrawCard(s_tft, TILE_X, y, TILE_W, TILE_H, accent);

  // Icon well — 56×56 square on the left, centered vertically.
  const int16_t iconCX = TILE_X + 18 + 28;     // accent stripe + pad + half
  const int16_t iconCY = y + TILE_H / 2;
  iconFn(iconCX, iconCY);

  // Thin vertical separator between the icon well and the label column.
  const int16_t sepX = TILE_X + 82;
  s_tft->fillRect(sepX, y + 16, 1, TILE_H - 32, UI_C_CARD_HI);

  // Label — big and bright, vertically centered. FreeSansBold 18pt gives
  // enough weight that the tile reads as a proper button.
  s_tft->setFreeFont(&FreeSansBold18pt7b);
  s_tft->setTextDatum(ML_DATUM);
  s_tft->setTextColor(UI_C_TEXT, UI_C_CARD);
  s_tft->drawString(label, sepX + 12, y + TILE_H / 2);

  // Chevron ">" on the right edge, vertically centered, in the tile's
  // accent color so the affordance matches the stripe.
  s_tft->setFreeFont(&FreeSansBold18pt7b);
  s_tft->setTextDatum(MR_DATUM);
  s_tft->setTextColor(accent, UI_C_CARD);
  s_tft->drawString(">", TILE_X + TILE_W - 16, y + TILE_H / 2);
  s_tft->setFreeFont(nullptr);
}

// ---------------------------------------------------------------------------
// Status bar — MENU label, centered price ticker, X close button.
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, 30, UI_C_BG);

  s_tft->setFreeFont(nullptr);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(UI_C_ACCENT, UI_C_BG);
  s_tft->setCursor(10, TOPBAR_Y + 4);
  s_tft->print("MENU");

  String price = priceDisplayString();
  if (price.length() > 0) {
    s_tft->setTextDatum(TC_DATUM);
    s_tft->setTextColor(UI_C_ACCENT_HI, UI_C_BG);
    s_tft->drawString(price, SCR_W / 2, TOPBAR_Y + 4);
  }
  s_lastPrice = price;

  screenfxDrawXButton(s_tft, CLOSE_X, CLOSE_Y, UI_C_ACCENT, UI_C_BG);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool menuScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

static void fullPaint() {
  s_tft->fillScreen(UI_C_BG);
  paintStatusBar();
  paintTile(TILE1_Y, "Wallet", UI_C_ACCENT, drawWalletIcon);
  paintTile(TILE2_Y, "Info",   UI_C_WARN,   drawInfoIcon);
}

static void paintIntoSprite(TFT_eSPI *sprite) {
  TFT_eSPI *real = s_tft;
  s_tft = sprite;
  fullPaint();
  s_tft = real;
}

void menuScreenOnEnter() {
  s_wantClose     = false;
  s_wantWalletTap = false;
  s_wantInfoTap   = false;
  s_lastPrice     = "";
  if (!screenfxSlideIn(s_tft, +SCR_H, paintIntoSprite)) {
    fullPaint();
  }
}

void menuScreenDraw() {
  if (!s_tft) return;
  fullPaint();
}

void menuScreenTick() {
  if (!s_tft) return;

  // Tiles are static. The only thing that changes on this screen is the
  // SOL/USD price ticker, and we only need to touch the status bar when
  // the displayed string actually changes. No tile repaints → no flicker.
  uint32_t now = millis();
  if (now - s_lastTickMs < 500) return;
  s_lastTickMs = now;

  String price = priceDisplayString();
  if (price != s_lastPrice) {
    paintStatusBar();
  }
}

void menuScreenHandleTap(int16_t x, int16_t y) {
  if (!s_tft) return;

  // X → home.
  if (x >= CLOSE_X && x < CLOSE_X + CLOSE_W &&
      y >= CLOSE_Y && y < CLOSE_Y + CLOSE_H) {
    s_wantClose = true;
    return;
  }
  // Wallet tile.
  if (x >= TILE_X && x < TILE_X + TILE_W &&
      y >= TILE1_Y && y < TILE1_Y + TILE_H) {
    s_wantWalletTap = true;
    return;
  }
  // Info tile.
  if (x >= TILE_X && x < TILE_X + TILE_W &&
      y >= TILE2_Y && y < TILE2_Y + TILE_H) {
    s_wantInfoTap = true;
    return;
  }
}

bool menuScreenConsumeClose() {
  if (!s_wantClose) return false;
  s_wantClose = false;
  return true;
}

bool menuScreenConsumeWalletTap() {
  if (!s_wantWalletTap) return false;
  s_wantWalletTap = false;
  return true;
}

bool menuScreenConsumeInfoTap() {
  if (!s_wantInfoTap) return false;
  s_wantInfoTap = false;
  return true;
}
