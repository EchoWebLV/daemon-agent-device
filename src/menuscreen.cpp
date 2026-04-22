#include "menuscreen.h"
#include "screenfx.h"
#include "wallet.h"
#include "price.h"

static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;

// Palette (RGB565) — kept in lockstep with walletscreen.cpp so the top
// bars and accents match across panels.
static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_PANEL     = 0x1082;   // dark slate card body
static constexpr uint16_t C_PANEL_HI  = 0x2124;   // slightly brighter, for accents
static constexpr uint16_t C_ACCENT    = 0x0AFF;   // Daemon blue
static constexpr uint16_t C_ACCENT_HI = 0x07FF;   // cyan rim
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_DIM       = 0x7BEF;
static constexpr uint16_t C_DIMMER    = 0x39C7;

// Top bar geometry — match walletscreen so the two sheets align visually.
static constexpr int16_t TOPBAR_Y = 3;
static constexpr int16_t CLOSE_W  = SCREENFX_X_BTN_W;
static constexpr int16_t CLOSE_H  = SCREENFX_X_BTN_H;
static constexpr int16_t CLOSE_X  = SCR_W - CLOSE_W - 6;
static constexpr int16_t CLOSE_Y  = TOPBAR_Y;

// Tile geometry. Two full-width cards, stacked vertically with equal
// breathing room above, between and below.
static constexpr int16_t TILE_X   = 14;
static constexpr int16_t TILE_W   = SCR_W - 2 * TILE_X;
static constexpr int16_t TILE_H   = 120;
static constexpr int16_t TILE1_Y  = 44;
static constexpr int16_t TILE2_Y  = TILE1_Y + TILE_H + 14;   // 178

static TFT_eSPI *s_tft = nullptr;

// Latched intents.
static bool s_wantClose      = false;
static bool s_wantWalletTap  = false;
static bool s_wantInfoTap    = false;

// Cached preview state so we only redraw tiles when their subtitles change.
static double   s_lastSol      = -1.0;
static double   s_lastUsdc     = -1.0;
static uint32_t s_lastHeapKB   = 0;
static uint32_t s_lastTickMs   = 0;

// ---------------------------------------------------------------------------
// Icon painters — drawn programmatically so we don't need to ship images.
// ---------------------------------------------------------------------------

// Wallet icon: a little credit-card-shaped rectangle with a "$" glyph.
static void drawWalletIcon(int16_t cx, int16_t cy) {
  const int16_t w = 46, h = 32;
  const int16_t x = cx - w / 2;
  const int16_t y = cy - h / 2;
  s_tft->fillRoundRect(x, y, w, h, 5, C_ACCENT);
  // A thin inner border for depth.
  s_tft->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 4, C_ACCENT_HI);
  // "$" centered
  s_tft->setTextFont(4);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(C_BG, C_ACCENT);
  s_tft->drawString("$", cx, cy + 1);
}

// Info icon: filled circle with an italic "i" glyph.
static void drawInfoIcon(int16_t cx, int16_t cy) {
  const int16_t r = 20;
  s_tft->fillCircle(cx, cy, r, C_ACCENT);
  s_tft->drawCircle(cx, cy, r, C_ACCENT_HI);
  s_tft->setTextFont(4);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(C_BG, C_ACCENT);
  s_tft->drawString("i", cx, cy + 1);
}

// ---------------------------------------------------------------------------
// Card painter — shared by both tiles.
// ---------------------------------------------------------------------------
typedef void (*IconFn)(int16_t, int16_t);

static void paintTile(int16_t y,
                      const char *title,
                      const char *subtitle,
                      IconFn iconFn) {
  // Background card.
  s_tft->fillRoundRect(TILE_X, y, TILE_W, TILE_H, 10, C_PANEL);
  s_tft->drawRoundRect(TILE_X, y, TILE_W, TILE_H, 10, C_PANEL_HI);

  // Icon zone: 60-px-wide strip on the left, centered vertically.
  const int16_t iconCX = TILE_X + 36;
  const int16_t iconCY = y + TILE_H / 2;
  iconFn(iconCX, iconCY);

  // Title — big accent label.
  s_tft->setTextFont(4);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_TEXT, C_PANEL);
  s_tft->setCursor(TILE_X + 74, y + 26);
  s_tft->print(title);

  // Subtitle — dim one-line preview.
  s_tft->setTextFont(2);
  s_tft->setTextColor(C_DIM, C_PANEL);
  s_tft->setCursor(TILE_X + 74, y + 58);
  s_tft->print(subtitle);

  // Chevron ">" on the right hinting the card is tappable.
  s_tft->setTextFont(4);
  s_tft->setTextDatum(MR_DATUM);
  s_tft->setTextColor(C_ACCENT_HI, C_PANEL);
  s_tft->drawString(">", TILE_X + TILE_W - 14, y + TILE_H / 2);
}

// ---------------------------------------------------------------------------
// Top bar — title centered, price ticker on the right, X close button.
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, 30, C_BG);

  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(10, TOPBAR_Y + 4);
  s_tft->print("MENU");

  // Centered price ticker, if we have one.
  String price = priceDisplayString();
  if (price.length() > 0) {
    s_tft->setTextDatum(TC_DATUM);
    s_tft->setTextColor(C_ACCENT_HI, C_BG);
    s_tft->drawString(price, SCR_W / 2, TOPBAR_Y + 4);
  }

  screenfxDrawXButton(s_tft, CLOSE_X, CLOSE_Y, C_ACCENT, C_BG);
}

// Build the wallet tile subtitle: "0.090 SOL · 12,345 USDC" etc.
static String walletSubtitle() {
  char buf[64];
  double sol  = walletSolBalance();
  double usdc = walletUsdcAmount();
  if (sol > 0 && usdc > 0) {
    snprintf(buf, sizeof(buf), "%.3f SOL  ·  %.2f USDC", sol, usdc);
  } else if (sol > 0) {
    snprintf(buf, sizeof(buf), "%.3f SOL", sol);
  } else {
    snprintf(buf, sizeof(buf), "balance loading…");
  }
  return String(buf);
}

// Build the info tile subtitle: "heap free · uptime".
static String infoSubtitle() {
  char buf[64];
  uint32_t heapKB = ESP.getFreeHeap() / 1024;
  uint32_t up = millis() / 1000;
  uint32_t m = up / 60, s = up % 60;
  if (m >= 60) {
    snprintf(buf, sizeof(buf), "%u KB free  ·  up %uh %um",
             (unsigned)heapKB, (unsigned)(m / 60), (unsigned)(m % 60));
  } else {
    snprintf(buf, sizeof(buf), "%u KB free  ·  up %um %us",
             (unsigned)heapKB, (unsigned)m, (unsigned)s);
  }
  return String(buf);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool menuScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

static void fullPaint() {
  s_tft->fillScreen(C_BG);
  paintStatusBar();
  paintTile(TILE1_Y, "Wallet", walletSubtitle().c_str(), drawWalletIcon);
  paintTile(TILE2_Y, "Info",   infoSubtitle().c_str(),   drawInfoIcon);

  s_lastSol    = walletSolBalance();
  s_lastUsdc   = walletUsdcAmount();
  s_lastHeapKB = ESP.getFreeHeap() / 1024;
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

  // Refresh subtitles at ~2 Hz so balances and uptime stay current
  // without flicker.
  uint32_t now = millis();
  if (now - s_lastTickMs < 500) return;
  s_lastTickMs = now;

  bool walletChanged =
      (walletSolBalance()  != s_lastSol) ||
      (walletUsdcAmount()  != s_lastUsdc);
  bool infoChanged =
      ((ESP.getFreeHeap() / 1024) != s_lastHeapKB) ||
      true;  // uptime always changes; always repaint info tile.

  if (walletChanged) {
    paintTile(TILE1_Y, "Wallet", walletSubtitle().c_str(), drawWalletIcon);
    s_lastSol  = walletSolBalance();
    s_lastUsdc = walletUsdcAmount();
  }
  if (infoChanged) {
    paintTile(TILE2_Y, "Info",   infoSubtitle().c_str(),   drawInfoIcon);
    s_lastHeapKB = ESP.getFreeHeap() / 1024;
  }
  // Keep the price ticker fresh.
  paintStatusBar();
}

void menuScreenHandleTap(int16_t x, int16_t y) {
  if (!s_tft) return;

  // X close button → home.
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
