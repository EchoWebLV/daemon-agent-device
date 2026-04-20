#include "menuscreen.h"
#include "moltbook.h"
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

// Four stacked tiles filling the body of the screen. 200×60 each with an
// 8-px gap fits between the 24-px status bar and the footer at y=306.
static constexpr int16_t TILE_W         = 200;
static constexpr int16_t TILE_H         = 60;
static constexpr int16_t TILE_ORIGIN_X  = (SCR_W - TILE_W) / 2;
static constexpr int16_t TILE_WALLET_Y  = 30;
static constexpr int16_t TILE_XPOST_Y   = TILE_WALLET_Y   + TILE_H + 8;
static constexpr int16_t TILE_MOLTBOOK_Y= TILE_XPOST_Y    + TILE_H + 8;
static constexpr int16_t TILE_INFO_Y    = TILE_MOLTBOOK_Y + TILE_H + 8;

// Index of each tile for tap hit-testing.
enum TileId : uint8_t {
  TILE_ID_WALLET   = 0,
  TILE_ID_XPOST    = 1,
  TILE_ID_MOLTBOOK = 2,
  TILE_ID_INFO     = 3,
  TILE_ID_NONE     = 255,
};

static TFT_eSPI *s_tft = nullptr;

// Edge-triggered taps consumed by main.
static bool s_wantWalletTap   = false;
static bool s_wantXTap        = false;
static bool s_wantMoltbookTap = false;
static bool s_wantInfoTap     = false;
static bool s_wantClose       = false;

// Press / release tap detection — same pattern as settingsscreen.cpp so
// the start of a SWIPE_DOWN gesture that happens to land on a tile
// doesn't fire the tile's action.
static bool     s_pressed     = false;
static int16_t  s_pressX      = 0;
static int16_t  s_pressY      = 0;
static bool     s_pressMoved  = false;
static constexpr int16_t TAP_MOVE_BUDGET = 14;

// Visual "pressed" highlight: which tile (if any) is currently being held.
static uint8_t  s_heldTile      = TILE_ID_NONE;
static uint8_t  s_lastDrawnHeld = TILE_ID_NONE;
static String   s_lastPrice     = "\x01";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool inRect(int16_t x, int16_t y,
                   int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int16_t tileY(uint8_t id) {
  switch (id) {
    case TILE_ID_WALLET:   return TILE_WALLET_Y;
    case TILE_ID_XPOST:    return TILE_XPOST_Y;
    case TILE_ID_MOLTBOOK: return TILE_MOLTBOOK_Y;
    case TILE_ID_INFO:     return TILE_INFO_Y;
    default:               return 0;
  }
}

static uint8_t hitTile(int16_t x, int16_t y) {
  if (inRect(x, y, TILE_ORIGIN_X, TILE_WALLET_Y,   TILE_W, TILE_H)) return TILE_ID_WALLET;
  if (inRect(x, y, TILE_ORIGIN_X, TILE_XPOST_Y,    TILE_W, TILE_H)) return TILE_ID_XPOST;
  if (inRect(x, y, TILE_ORIGIN_X, TILE_MOLTBOOK_Y, TILE_W, TILE_H)) return TILE_ID_MOLTBOOK;
  if (inRect(x, y, TILE_ORIGIN_X, TILE_INFO_Y,     TILE_W, TILE_H)) return TILE_ID_INFO;
  return TILE_ID_NONE;
}

// ---------------------------------------------------------------------------
// Icon painters — we render icons as vector primitives so they scale
// cleanly with the tile size and don't need any bitmap assets.
// ---------------------------------------------------------------------------
static void drawWalletIcon(int16_t cx, int16_t cy, uint16_t color) {
  // A classic "wallet" glyph: a rounded-rect body with a secondary fold
  // line across the middle and a small clasp/button dot on the right edge.
  const int16_t w = 48;
  const int16_t h = 34;
  const int16_t x = cx - w / 2;
  const int16_t y = cy - h / 2;
  s_tft->drawRoundRect(x,     y,     w,     h,     6, color);
  s_tft->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, color);
  s_tft->drawFastHLine(x + 4, y + 12, w - 8, color);
  s_tft->drawFastHLine(x + 6, y + 19, w - 12, color);
  s_tft->fillCircle(x + w - 9, y + h / 2 + 5, 3, color);
}

static void drawXIcon(int16_t cx, int16_t cy, uint16_t color) {
  // Bold diagonal "X" — mirrors the X corporate mark. Drawn as two
  // diagonal strokes, each ~3 px thick via a 3×3 stamp of drawLine
  // calls (cheap on TFT_eSPI, and crisp enough for a 240-px screen).
  const int16_t r = 17;
  for (int dx = -1; dx <= 1; ++dx) {
    for (int dy = -1; dy <= 1; ++dy) {
      s_tft->drawLine(cx - r + dx, cy - r + dy, cx + r + dx, cy + r + dy, color);
      s_tft->drawLine(cx - r + dx, cy + r + dy, cx + r + dx, cy - r + dy, color);
    }
  }
}

static void drawMoltbookIcon(int16_t cx, int16_t cy, uint16_t color) {
  // Stylised "M" mark for Moltbook — bold and recognisable at 240px.
  // Two thick diagonal strokes forming an M shape.
  const int16_t h = 20;
  const int16_t w = 22;
  for (int d = -1; d <= 1; ++d) {
    // Left leg
    s_tft->drawLine(cx - w + d, cy + h, cx - w + d, cy - h, color);
    // Left peak
    s_tft->drawLine(cx - w + d, cy - h, cx + d,     cy,     color);
    // Right peak
    s_tft->drawLine(cx + d,     cy,     cx + w + d, cy - h, color);
    // Right leg
    s_tft->drawLine(cx + w + d, cy - h, cx + w + d, cy + h, color);
  }
}

static void drawInfoIcon(int16_t cx, int16_t cy, uint16_t color) {
  // Lowercase "i" inside a double-stroke circle.
  const int16_t r = 18;
  s_tft->drawCircle(cx, cy, r,     color);
  s_tft->drawCircle(cx, cy, r - 1, color);
  s_tft->fillCircle(cx, cy - 8, 2, color);
  s_tft->fillRect(cx - 2, cy - 2, 4, 12, color);
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

  String price = priceDisplayString();
  if (price.length() > 0) {
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_ACCENT_HI, C_BG);
    s_tft->drawString(price, CLOSE_X - 4, 4);
  }
  s_lastPrice = price;

  statusIconsDraw(s_tft, STATUS_H + 60, STATUS_H / 2, C_ACCENT_HI, C_BG);

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
  const int16_t  y      = tileY(id);
  const bool     held   = (s_heldTile == id);
  const uint16_t fill   = held ? C_CARD_HI : C_CARD;
  const uint16_t stroke = held ? C_ACCENT_HI : C_ACCENT;

  const char *label; const char *hint;
  switch (id) {
    case TILE_ID_WALLET:   label = "Wallet";   hint = "address + balances";   break;
    case TILE_ID_XPOST:    label = "X";        hint = "recent posted tweets"; break;
    case TILE_ID_MOLTBOOK: label = "Moltbook"; hint = "AI agent social net";  break;
    case TILE_ID_INFO:     label = "Info";     hint = "ip, firmware, uptime"; break;
    default:               return;
  }

  s_tft->fillRoundRect(TILE_ORIGIN_X, y, TILE_W, TILE_H, 12, fill);
  s_tft->drawRoundRect(TILE_ORIGIN_X, y, TILE_W, TILE_H, 12, stroke);
  s_tft->drawRoundRect(TILE_ORIGIN_X + 1, y + 1, TILE_W - 2, TILE_H - 2, 11, stroke);

  const int16_t iconCx = TILE_ORIGIN_X + 40;
  const int16_t iconCy = y + TILE_H / 2;
  if      (id == TILE_ID_WALLET)   drawWalletIcon(iconCx, iconCy, stroke);
  else if (id == TILE_ID_XPOST)    drawXIcon(iconCx, iconCy, stroke);
  else if (id == TILE_ID_MOLTBOOK) drawMoltbookIcon(iconCx, iconCy, stroke);
  else                             drawInfoIcon(iconCx, iconCy, stroke);

  s_tft->setTextFont(4);
  s_tft->setTextDatum(ML_DATUM);
  s_tft->setTextColor(C_TEXT, fill);
  s_tft->drawString(label, TILE_ORIGIN_X + 82, y + TILE_H / 2 - 6);

  s_tft->setTextFont(1);
  s_tft->setTextColor(C_DIM, fill);
  s_tft->drawString(hint, TILE_ORIGIN_X + 82, y + TILE_H / 2 + 14);
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
      s_heldTile   = TILE_ID_NONE;
    }
  } else if (s_pressed) {
    s_pressed = false;
    uint8_t held = s_heldTile;
    s_heldTile = TILE_ID_NONE;
    if (s_pressMoved) return;

    const int16_t tx = s_pressX, ty = s_pressY;
    if (inRect(tx, ty, CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H)) {
      s_wantClose = true;
      return;
    }
    if (held == TILE_ID_WALLET)   { s_wantWalletTap   = true; return; }
    if (held == TILE_ID_XPOST)    { s_wantXTap        = true; return; }
    if (held == TILE_ID_MOLTBOOK) { s_wantMoltbookTap = true; return; }
    if (held == TILE_ID_INFO)     { s_wantInfoTap     = true; return; }
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
  paintTile(TILE_ID_WALLET);
  paintTile(TILE_ID_XPOST);
  paintTile(TILE_ID_MOLTBOOK);
  paintTile(TILE_ID_INFO);
  paintFooter();
  s_lastDrawnHeld = s_heldTile;
}

void menuScreenDrawTo(TFT_eSprite *target) {
  if (!target) return;
  TFT_eSPI *saved = s_tft;
  s_tft = target;          // every paint helper writes through s_tft
  menuScreenDraw();
  s_tft = saved;
  // Mark the status-icon cache stale so the next live tick repaints
  // it on the real TFT instead of skipping (we just painted to the
  // sprite, which counts as "drew the icons" from its POV).
  statusIconsResetCache();
}

void menuScreenTick() {
  if (!s_tft) return;
  handleInput();

  String price = priceDisplayString();
  if (price != s_lastPrice || statusIconsNeedRedraw()) {
    paintStatusBar();
  }

  if (s_heldTile != s_lastDrawnHeld) {
    // Repaint both the tile that just gained focus and the tile that just
    // lost it so the highlight swap is visually instant.
    if (s_lastDrawnHeld != TILE_ID_NONE) paintTile(s_lastDrawnHeld);
    if (s_heldTile      != TILE_ID_NONE) paintTile(s_heldTile);
    s_lastDrawnHeld = s_heldTile;
  }
}

bool menuScreenConsumeWalletTap() {
  if (!s_wantWalletTap) return false;
  s_wantWalletTap = false;
  return true;
}

bool menuScreenConsumeXTap() {
  if (!s_wantXTap) return false;
  s_wantXTap = false;
  return true;
}

bool menuScreenConsumeMoltbookTap() {
  if (!s_wantMoltbookTap) return false;
  s_wantMoltbookTap = false;
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
