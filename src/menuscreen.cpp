#include "menuscreen.h"
#include "screenfx.h"
#include "wallet.h"
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

// Two full-width tiles stacked vertically, each ~116 px tall with
// breathing room above / between / below.
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

// Subtitle change detection so we only repaint tiles when needed.
static double   s_lastSol      = -1.0;
static double   s_lastUsdc     = -1.0;
static uint32_t s_lastHeapKB   = 0;
static uint32_t s_lastTickMs   = 0;

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

  // Subtle drop shadow for perceived depth.
  s_tft->fillRoundRect(x + 1, y + 2, w, h, 7, UI_C_BG_DEEP);
  // Card body.
  s_tft->fillRoundRect(x, y, w, h, 7, UI_C_ACCENT);
  // Inner bevel.
  s_tft->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 5, UI_C_ACCENT_HI);
  // Horizontal magnetic-stripe band across the top third.
  s_tft->fillRect(x + 4, y + 8, w - 8, 3, UI_C_BG_DEEP);
  // "$" glyph, bold, centered in the lower half of the card.
  s_tft->setFreeFont(&FreeSansBold12pt7b);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(UI_C_TEXT, UI_C_ACCENT);
  s_tft->drawString("$", cx, cy + 5);
  s_tft->setFreeFont(nullptr);
}

// Info icon: a two-ring amber disc with a lowercase "i" glyph. The
// concentric rings are what sells the "badge" read at a glance.
static void drawInfoIcon(int16_t cx, int16_t cy) {
  const int16_t r = 22;
  // Drop shadow.
  s_tft->fillCircle(cx + 1, cy + 2, r, UI_C_BG_DEEP);
  // Outer disc.
  s_tft->fillCircle(cx, cy, r, UI_C_WARN);
  // Outer rim.
  s_tft->drawCircle(cx, cy, r, UI_C_WARN_DIM);
  // Inner ring for extra depth.
  s_tft->drawCircle(cx, cy, r - 4, UI_C_WARN_DIM);
  // "i" — a small dot above, a longer stem below.
  s_tft->fillCircle(cx, cy - 8, 2, UI_C_BG_DEEP);
  s_tft->fillRect(cx - 2, cy - 3, 4, 12, UI_C_BG_DEEP);
}

// ---------------------------------------------------------------------------
// Tile painter — shared layout with per-tile icon / title / subtitle /
// accent color.
// ---------------------------------------------------------------------------
typedef void (*IconFn)(int16_t, int16_t);

static void paintTile(int16_t y,
                      const char *title,
                      const char *subtitle,
                      uint16_t    accent,
                      IconFn      iconFn) {
  // Wipe the tile slot first so re-renders don't leave artifacts.
  s_tft->fillRect(TILE_X, y - 4, TILE_W, TILE_H + 8, UI_C_BG);

  // The shared card chrome — shadow + body + highlight border + left
  // accent stripe.
  screenfxDrawCard(s_tft, TILE_X, y, TILE_W, TILE_H, accent);

  // Icon well — 56×56 square on the left, centered vertically.
  const int16_t iconCX = TILE_X + 18 + 28;     // accent stripe + pad + half
  const int16_t iconCY = y + TILE_H / 2;
  iconFn(iconCX, iconCY);

  // Thin vertical separator between the icon well and the text column.
  const int16_t sepX = TILE_X + 82;
  s_tft->fillRect(sepX, y + 16, 1, TILE_H - 32, UI_C_CARD_HI);

  // Title — big, bright, FreeSansBold 12pt for a solid header weight.
  s_tft->setFreeFont(&FreeSansBold12pt7b);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(UI_C_TEXT, UI_C_CARD);
  s_tft->setCursor(sepX + 10, y + 26);
  s_tft->print(title);

  // Subtitle — FreeSans 9pt in dim text color. Rendered as a single line
  // without wrapping; callers keep the subtitle short.
  s_tft->setFreeFont(&FreeSans9pt7b);
  s_tft->setTextColor(UI_C_TEXT_DIM, UI_C_CARD);
  s_tft->setCursor(sepX + 10, y + 58);
  s_tft->print(subtitle);

  // Accent mini-label under subtitle: "Tap to open" in the tile's accent
  // color, a hair larger than the built-in small font for readability.
  s_tft->setFreeFont(nullptr);
  s_tft->setTextFont(1);
  s_tft->setTextColor(accent, UI_C_CARD);
  s_tft->setCursor(sepX + 10, y + TILE_H - 18);
  s_tft->print("TAP TO OPEN");

  // Chevron ">" on the right edge, vertically centered.
  s_tft->setFreeFont(&FreeSansBold18pt7b);
  s_tft->setTextDatum(MR_DATUM);
  s_tft->setTextColor(UI_C_ACCENT_HI, UI_C_CARD);
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

  screenfxDrawXButton(s_tft, CLOSE_X, CLOSE_Y, UI_C_ACCENT, UI_C_BG);
}

// ---------------------------------------------------------------------------
// Subtitle builders
// ---------------------------------------------------------------------------
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

static String infoSubtitle() {
  char buf[64];
  uint32_t heapKB = ESP.getFreeHeap() / 1024;
  uint32_t up = millis() / 1000;
  uint32_t m = up / 60, s = up % 60;
  if (m >= 60) {
    snprintf(buf, sizeof(buf), "%u KB free  ·  %uh %um up",
             (unsigned)heapKB, (unsigned)(m / 60), (unsigned)(m % 60));
  } else {
    snprintf(buf, sizeof(buf), "%u KB free  ·  %um %us up",
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
  s_tft->fillScreen(UI_C_BG);
  paintStatusBar();

  paintTile(TILE1_Y, "Wallet", walletSubtitle().c_str(),
            UI_C_ACCENT, drawWalletIcon);
  paintTile(TILE2_Y, "Info",   infoSubtitle().c_str(),
            UI_C_WARN,   drawInfoIcon);

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

  uint32_t now = millis();
  if (now - s_lastTickMs < 500) return;     // ~2 Hz
  s_lastTickMs = now;

  bool walletChanged =
      (walletSolBalance() != s_lastSol) ||
      (walletUsdcAmount() != s_lastUsdc);
  // Uptime always ticks, so the info tile always refreshes.
  bool infoChanged = true;

  if (walletChanged) {
    paintTile(TILE1_Y, "Wallet", walletSubtitle().c_str(),
              UI_C_ACCENT, drawWalletIcon);
    s_lastSol  = walletSolBalance();
    s_lastUsdc = walletUsdcAmount();
  }
  if (infoChanged) {
    paintTile(TILE2_Y, "Info", infoSubtitle().c_str(),
              UI_C_WARN, drawInfoIcon);
    s_lastHeapKB = ESP.getFreeHeap() / 1024;
  }

  // Keep the price ticker current.
  paintStatusBar();
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
