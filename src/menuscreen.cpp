#include "menuscreen.h"
#include "price.h"
#include "statusicons.h"
#include "touch.h"

// ---------------------------------------------------------------------------
// Geometry / palette — same dark Daemon look as the other screens so the
// menu feels like part of the same UI, not a pop-up from a different app.
// ---------------------------------------------------------------------------
static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 16;

static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_CARD      = 0x0861;   // subtle navy card
static constexpr uint16_t C_CARD_HI   = 0x18E3;   // brighter on press
static constexpr uint16_t C_ACCENT    = 0x0AFF;
static constexpr uint16_t C_ACCENT_HI = 0x07FF;
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_DIM       = 0x7BEF;

// Close "X" button geometry (matches settings / wifi screens).
static constexpr int16_t CLOSE_W = 36;
static constexpr int16_t CLOSE_H = 24;
static constexpr int16_t CLOSE_X = SCR_W - CLOSE_W;
static constexpr int16_t CLOSE_Y = 0;

// Two stacked tiles filling the body of the screen. 200×100 each with a
// 30-px gap keeps them inside the 240-px width with generous margins and
// vertically centred between the status bar and the "swipe down" footer.
static constexpr int16_t TILE_W        = 200;
static constexpr int16_t TILE_H        = 104;
static constexpr int16_t TILE_X        = (SCR_W - TILE_W) / 2;
static constexpr int16_t TILE_WALLET_Y = 44;
static constexpr int16_t TILE_INFO_Y   = TILE_WALLET_Y + TILE_H + 20;   // 168

// Index of each tile for tap hit-testing.
enum TileId : uint8_t { TILE_WALLET = 0, TILE_INFO = 1, TILE_NONE = 255 };

static TFT_eSPI *s_tft = nullptr;

// Edge-triggered taps consumed by main.
static bool s_wantWalletTap = false;
static bool s_wantInfoTap   = false;
static bool s_wantClose     = false;

// Press / release tap detection — same pattern as settingsscreen.cpp so
// the start of a SWIPE_DOWN gesture that happens to land on a tile
// doesn't fire the tile's action.
static bool     s_pressed     = false;
static int16_t  s_pressX      = 0;
static int16_t  s_pressY      = 0;
static bool     s_pressMoved  = false;
static constexpr int16_t TAP_MOVE_BUDGET = 14;

// Visual "pressed" highlight: which tile (if any) is currently being held.
static uint8_t  s_heldTile    = TILE_NONE;
static uint8_t  s_lastDrawnHeld = TILE_NONE;
static String   s_lastPrice   = "\x01";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool inRect(int16_t x, int16_t y,
                   int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static uint8_t hitTile(int16_t x, int16_t y) {
  if (inRect(x, y, TILE_X, TILE_WALLET_Y, TILE_W, TILE_H)) return TILE_WALLET;
  if (inRect(x, y, TILE_X, TILE_INFO_Y,   TILE_W, TILE_H)) return TILE_INFO;
  return TILE_NONE;
}

// ---------------------------------------------------------------------------
// Icon painters — we render both icons as vector primitives so they scale
// cleanly with the tile size and don't need any bitmap assets.
// ---------------------------------------------------------------------------
static void drawWalletIcon(int16_t cx, int16_t cy, uint16_t color) {
  // A classic "wallet" glyph: a rounded-rect body with a secondary fold
  // line across the middle and a small clasp/button dot on the right edge.
  const int16_t w = 54;
  const int16_t h = 38;
  const int16_t x = cx - w / 2;
  const int16_t y = cy - h / 2;
  s_tft->drawRoundRect(x,     y,     w,     h,     6, color);
  s_tft->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, color);
  // Flap fold — horizontal ridge a third of the way down.
  s_tft->drawFastHLine(x + 4, y + 13, w - 8, color);
  // Card-slot hint just below the fold.
  s_tft->drawFastHLine(x + 6, y + 20, w - 12, color);
  // Clasp/button on the right edge.
  s_tft->fillCircle(x + w - 10, y + h / 2 + 5, 3, color);
}

static void drawInfoIcon(int16_t cx, int16_t cy, uint16_t color) {
  // Lowercase "i" inside a double-stroke circle.
  const int16_t r = 20;
  s_tft->drawCircle(cx, cy, r,     color);
  s_tft->drawCircle(cx, cy, r - 1, color);
  // dot of the i
  s_tft->fillCircle(cx, cy - 9, 2, color);
  // stem of the i (thicker so it reads at a glance)
  s_tft->fillRect(cx - 2, cy - 3, 4, 14, color);
}

// ---------------------------------------------------------------------------
// Painters
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, CLOSE_H, C_BG);
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(4, 4);
  s_tft->print("MENU");

  // Price ticker on the right, just left of the close button.
  String price = priceDisplayString();
  if (price.length() > 0) {
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_ACCENT_HI, C_BG);
    s_tft->drawString(price, CLOSE_X - 4, 4);
  }
  s_lastPrice = price;

  // Feature indicators (memory / heartbeat) — keep parity with other screens.
  statusIconsDraw(s_tft, STATUS_H + 60, STATUS_H / 2, C_ACCENT_HI, C_BG);

  // Close box.
  s_tft->drawRoundRect(CLOSE_X + 2, CLOSE_Y + 2, CLOSE_W - 4, CLOSE_H - 4, 3, C_ACCENT);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->drawString("x", CLOSE_X + CLOSE_W / 2, CLOSE_Y + CLOSE_H / 2);
}

static void paintFooter() {
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString("swipe down to close", SCR_W / 2, SCR_H - 14);
}

static void paintTile(uint8_t id) {
  const int16_t  y          = (id == TILE_WALLET) ? TILE_WALLET_Y : TILE_INFO_Y;
  const bool     held       = (s_heldTile == id);
  const uint16_t fill       = held ? C_CARD_HI : C_CARD;
  const uint16_t stroke     = held ? C_ACCENT_HI : C_ACCENT;
  const char    *label      = (id == TILE_WALLET) ? "Wallet" : "Info";

  s_tft->fillRoundRect(TILE_X, y, TILE_W, TILE_H, 14, fill);
  s_tft->drawRoundRect(TILE_X, y, TILE_W, TILE_H, 14, stroke);
  // Inner stroke for a crisper edge.
  s_tft->drawRoundRect(TILE_X + 1, y + 1, TILE_W - 2, TILE_H - 2, 13, stroke);

  const int16_t iconCx = TILE_X + 46;
  const int16_t iconCy = y + TILE_H / 2;
  if (id == TILE_WALLET) drawWalletIcon(iconCx, iconCy, stroke);
  else                   drawInfoIcon(iconCx, iconCy, stroke);

  s_tft->setTextFont(4);
  s_tft->setTextDatum(ML_DATUM);
  s_tft->setTextColor(C_TEXT, fill);
  s_tft->drawString(label, TILE_X + 92, y + TILE_H / 2 - 4);

  s_tft->setTextFont(1);
  s_tft->setTextColor(C_DIM, fill);
  const char *hint = (id == TILE_WALLET) ? "address + balances"
                                         : "ip, firmware, uptime";
  s_tft->drawString(hint, TILE_X + 92, y + TILE_H / 2 + 18);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void handleInput() {
  int16_t x, y;
  bool pressed = touchActive(x, y);

  if (pressed) {
    if (!s_pressed) {
      s_pressed    = true;
      s_pressX     = x;
      s_pressY     = y;
      s_pressMoved = false;
      s_heldTile   = hitTile(x, y);
    } else if (abs(x - s_pressX) > TAP_MOVE_BUDGET ||
               abs(y - s_pressY) > TAP_MOVE_BUDGET) {
      s_pressMoved = true;
      s_heldTile   = TILE_NONE;    // moved out of tap range: drop highlight
    }
  } else if (s_pressed) {
    s_pressed = false;
    uint8_t held = s_heldTile;
    s_heldTile = TILE_NONE;
    if (s_pressMoved) return;      // was a swipe, not a tap

    const int16_t tx = s_pressX, ty = s_pressY;
    if (inRect(tx, ty, CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H)) {
      s_wantClose = true;
      return;
    }
    if (held == TILE_WALLET) { s_wantWalletTap = true; return; }
    if (held == TILE_INFO)   { s_wantInfoTap   = true; return; }
  }
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
bool menuScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

void menuScreenDraw() {
  if (!s_tft) return;
  s_tft->fillScreen(C_BG);
  statusIconsResetCache();
  paintStatusBar();
  paintTile(TILE_WALLET);
  paintTile(TILE_INFO);
  paintFooter();
  s_lastDrawnHeld = s_heldTile;
}

void menuScreenTick() {
  if (!s_tft) return;
  handleInput();

  // Only the status bar is dynamic (price + feature indicators). Repaint
  // when the ticker changes or the icons want a refresh.
  String price = priceDisplayString();
  if (price != s_lastPrice || statusIconsNeedRedraw()) {
    paintStatusBar();
  }

  // Repaint whichever tile(s) changed held-state this frame so the press
  // feedback is immediate without flooding the bus.
  if (s_heldTile != s_lastDrawnHeld) {
    if (s_lastDrawnHeld == TILE_WALLET || s_heldTile == TILE_WALLET) paintTile(TILE_WALLET);
    if (s_lastDrawnHeld == TILE_INFO   || s_heldTile == TILE_INFO)   paintTile(TILE_INFO);
    s_lastDrawnHeld = s_heldTile;
  }
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

bool menuScreenConsumeClose() {
  if (!s_wantClose) return false;
  s_wantClose = false;
  return true;
}
