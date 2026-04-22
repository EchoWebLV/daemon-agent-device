#include "walletscreen.h"
#include "wallet.h"
#include "price.h"
#include "base58.h"
#include "screenfx.h"

#include <qrcode.h>

static constexpr int16_t SCR_W    = SCREENFX_W;
static constexpr int16_t SCR_H    = SCREENFX_H;
static constexpr int16_t STATUS_H = 30;

// USDC mint — pinned to the top of the token list so the x402 balance is
// always the first thing the user sees.
static const char *USDC_MINT_B58 = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
// Top bar: QR on the left, address pill in the middle, X on the right.
static constexpr int16_t TOPBAR_Y = 3;
static constexpr int16_t QR_X     = 6;
static constexpr int16_t QR_W     = 28;
static constexpr int16_t QR_H     = 24;
static constexpr int16_t CLOSE_X  = SCR_W - SCREENFX_X_BTN_W - 6;
static constexpr int16_t CLOSE_Y  = TOPBAR_Y;
static constexpr int16_t CLOSE_W  = SCREENFX_X_BTN_W;
static constexpr int16_t CLOSE_H  = SCREENFX_X_BTN_H;

// Hero card — the big "your SOL balance" banner.
static constexpr int16_t HERO_X   = 10;
static constexpr int16_t HERO_Y   = 38;
static constexpr int16_t HERO_W   = SCR_W - 2 * HERO_X;
static constexpr int16_t HERO_H   = 112;

// Holdings section starts under the hero card.
static constexpr int16_t HOLDINGS_Y = HERO_Y + HERO_H + 10;   // 160

// Render-target pointer (swapped for the sprite during slide-in).
static TFT_eSPI *s_tft = nullptr;

// Tick-loop change detection.
static double   s_lastSolShown   = -1.0;
static double   s_lastPriceShown = -1.0;
static size_t   s_lastTokenCount = (size_t)-1;
static uint32_t s_lastTickMs     = 0;

// Latched intents.
static bool     s_wantClose      = false;

// Full-screen QR overlay — tap anywhere to dismiss.
static bool     s_qrOverlay      = false;

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
static String truncAddr(const String &p) {
  if (p.length() < 10) return p;
  return p.substring(0, 4) + "…" + p.substring(p.length() - 4);
}

static String fmtAmount(double a) {
  char buf[32];
  if (a >= 10000)    snprintf(buf, sizeof(buf), "%.0f", a);
  else if (a >= 100) snprintf(buf, sizeof(buf), "%.1f", a);
  else if (a >= 1)   snprintf(buf, sizeof(buf), "%.3f", a);
  else               snprintf(buf, sizeof(buf), "%.5f", a);
  return String(buf);
}

static String withCommas(const String &s) {
  int dot = s.indexOf('.');
  String intPart  = (dot < 0) ? s : s.substring(0, dot);
  String fracPart = (dot < 0) ? "" : s.substring(dot);
  bool neg = intPart.startsWith("-");
  if (neg) intPart.remove(0, 1);
  String out;
  int n = intPart.length();
  for (int i = 0; i < n; ++i) {
    if (i > 0 && (n - i) % 3 == 0) out += ',';
    out += intPart[i];
  }
  return (neg ? "-" : "") + out + fracPart;
}

// Deterministic per-mint color, floored at 0x60 per channel so we can draw
// readable dark glyphs on top.
static uint16_t colorForMint(const String &mintB58) {
  auto bytes = base58Decode(mintB58);
  uint8_t r = 0x80, g = 0x80, b = 0x80;
  if (bytes.size() >= 3) {
    r = bytes[0]; g = bytes[1]; b = bytes[2];
    if (r < 0x60) r = 0x60 + (r >> 1);
    if (g < 0x60) g = 0x60 + (g >> 1);
    if (b < 0x60) b = 0x60 + (b >> 1);
  }
  uint16_t r5 = (r >> 3) & 0x1F;
  uint16_t g6 = (g >> 2) & 0x3F;
  uint16_t b5 = (b >> 3) & 0x1F;
  return (r5 << 11) | (g6 << 5) | b5;
}

// ---------------------------------------------------------------------------
// QR icon — three position markers in a rounded frame.
// ---------------------------------------------------------------------------
static void drawQrIcon(int16_t x, int16_t y) {
  auto marker = [](TFT_eSPI *t, int16_t mx, int16_t my) {
    t->fillRect(mx,     my,     7, 7, UI_C_ACCENT);
    t->fillRect(mx + 1, my + 1, 5, 5, UI_C_BG);
    t->fillRect(mx + 2, my + 2, 3, 3, UI_C_ACCENT);
  };
  s_tft->fillRect(x, y, QR_W, QR_H, UI_C_BG);
  s_tft->drawRoundRect(x, y, QR_W, QR_H, 5, UI_C_ACCENT);
  const int16_t ox = x + 5, oy = y + 3;
  marker(s_tft, ox,     oy);
  marker(s_tft, ox + 9, oy);
  marker(s_tft, ox,     oy + 9);
  s_tft->fillRect(ox + 10, oy + 10, 3, 3, UI_C_ACCENT);
  s_tft->fillRect(ox + 14, oy + 10, 3, 3, UI_C_ACCENT);
  s_tft->fillRect(ox + 10, oy + 14, 3, 3, UI_C_ACCENT);
}

// ---------------------------------------------------------------------------
// Painters
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, STATUS_H, UI_C_BG);

  drawQrIcon(QR_X, TOPBAR_Y);
  screenfxDrawXButton(s_tft, CLOSE_X, CLOSE_Y, UI_C_ACCENT, UI_C_BG);

  // Address pill — tap the QR button to reveal the full address. The pill
  // itself is decorative / at-a-glance.
  const int16_t pillX = QR_X + QR_W + 6;
  const int16_t pillW = CLOSE_X - pillX - 6;
  const int16_t pillY = TOPBAR_Y + 2;
  const int16_t pillH = 20;
  s_tft->fillRoundRect(pillX, pillY, pillW, pillH, pillH / 2, UI_C_CARD_INSET);
  s_tft->drawRoundRect(pillX, pillY, pillW, pillH, pillH / 2, UI_C_CARD_HI);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(UI_C_TEXT, UI_C_CARD_INSET);
  s_tft->drawString(truncAddr(walletPubkey()),
                    pillX + pillW / 2,
                    pillY + pillH / 2 + 1);
}

// Hero balance card: the big SOL amount, the "SOL" label, and the USD
// valuation. Rendered with GFXFF FreeFonts so we're not relying on the
// 7-segment font 7 (which isn't compiled into TFT_eSPI on this build).
static void paintHeroCard() {
  screenfxDrawCard(s_tft, HERO_X, HERO_Y, HERO_W, HERO_H, UI_C_ACCENT);

  // Tiny "BALANCE" caption in the top-left of the card.
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(UI_C_ACCENT_HI, UI_C_CARD);
  s_tft->setCursor(HERO_X + 16, HERO_Y + 10);
  s_tft->print("BALANCE");

  // Hero SOL amount — GFXFF FreeSansBold 18pt, roughly 28 px tall.
  double sol = walletSolBalance();
  char solBuf[32];
  snprintf(solBuf, sizeof(solBuf), "%.3f", sol);

  const int16_t heroCX = HERO_X + HERO_W / 2;
  const int16_t heroNumY = HERO_Y + 26;

  s_tft->setFreeFont(&FreeSansBold18pt7b);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(UI_C_TEXT, UI_C_CARD);
  s_tft->drawString(solBuf, heroCX, heroNumY + 14);

  // Unit label under the number in accent cyan.
  s_tft->setFreeFont(&FreeSansBold9pt7b);
  s_tft->setTextColor(UI_C_ACCENT_HI, UI_C_CARD);
  s_tft->drawString("SOL", heroCX, heroNumY + 38);

  // USD valuation line at the bottom of the card.
  double p = priceSOLUSD();
  char usdBuf[32];
  if (p > 0) snprintf(usdBuf, sizeof(usdBuf), "≈ $%.2f", sol * p);
  else       snprintf(usdBuf, sizeof(usdBuf), "≈ $—");
  s_tft->setFreeFont(nullptr);         // back to built-in fonts
  s_tft->setTextFont(2);
  s_tft->setTextColor(UI_C_TEXT_DIM, UI_C_CARD);
  s_tft->drawString(usdBuf, heroCX, HERO_Y + HERO_H - 14);
}

// "HOLDINGS" section header — a divider line, an accent-colored label, and
// the token count on the right.
static void paintHoldingsHeader(int16_t y) {
  s_tft->fillRect(HERO_X, y, HERO_W, 1, UI_C_CARD_HI);

  s_tft->setFreeFont(nullptr);
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(UI_C_ACCENT, UI_C_BG);
  s_tft->setCursor(HERO_X + 2, y + 6);
  s_tft->print("HOLDINGS");

  char cbuf[16];
  snprintf(cbuf, sizeof(cbuf), "%u tokens",
           (unsigned)walletTokens().size());
  s_tft->setTextDatum(TR_DATUM);
  s_tft->setTextColor(UI_C_TEXT_DIMMER, UI_C_BG);
  s_tft->drawString(cbuf, HERO_X + HERO_W - 2, y + 6);
}

// Single token row. Returns the next-row y, or -1 if we're out of space.
static int paintTokenRow(const TokenHolding &t,
                         int16_t              y,
                         int16_t              yBottom) {
  constexpr int ROW_H = 32;
  if (y + ROW_H > yBottom) return -1;

  String symbol = (t.symbol.length() > 0) ? t.symbol
                                          : t.mint.substring(0, 4) + "…";
  String amount = withCommas(fmtAmount(t.amount));

  // Row background — a very subtle alternating band so rows read distinct
  // without loud stripes.
  uint16_t rowBg = UI_C_BG;
  s_tft->fillRect(HERO_X, y, HERO_W, ROW_H - 2, rowBg);

  // Coin chip: colored circle with a dark inset ring and the symbol's
  // first letter in the center.
  const int16_t chipR = 12;
  const int16_t chipX = HERO_X + chipR + 4;
  const int16_t chipY = y + ROW_H / 2 - 1;
  const uint16_t chipC = colorForMint(t.mint);
  s_tft->fillCircle(chipX, chipY, chipR, chipC);
  s_tft->drawCircle(chipX, chipY, chipR,     UI_C_CARD_HI);
  s_tft->drawCircle(chipX, chipY, chipR - 1, UI_C_BG_DEEP);

  // Letter inside the chip. Dark glyph on a light floor color reads well.
  s_tft->setFreeFont(&FreeSansBold9pt7b);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(UI_C_BG_DEEP, chipC);
  char letter[2] = { (char)toupper(symbol[0]), 0 };
  s_tft->drawString(letter, chipX, chipY + 1);

  // Symbol label, bright.
  s_tft->setFreeFont(&FreeSansBold9pt7b);
  s_tft->setTextDatum(ML_DATUM);
  s_tft->setTextColor(UI_C_TEXT, rowBg);
  s_tft->drawString(symbol, chipX + chipR + 10, chipY - 4);

  // Mint snippet under the symbol — tiny, dim.
  s_tft->setFreeFont(nullptr);
  s_tft->setTextFont(1);
  s_tft->setTextColor(UI_C_TEXT_DIMMER, rowBg);
  s_tft->drawString(truncAddr(t.mint), chipX + chipR + 10, chipY + 8);

  // Right-aligned amount in bright accent.
  s_tft->setFreeFont(&FreeSansBold9pt7b);
  s_tft->setTextDatum(MR_DATUM);
  s_tft->setTextColor(UI_C_ACCENT_HI, rowBg);
  s_tft->drawString(amount, HERO_X + HERO_W - 2, chipY);
  s_tft->setFreeFont(nullptr);

  // Subtle bottom divider between rows.
  s_tft->fillRect(HERO_X + 28, y + ROW_H - 1,
                  HERO_W - 28, 1, UI_C_CARD_INSET);

  return y + ROW_H;
}

static void paintTokens(int16_t yTop, int16_t yBottom) {
  const auto &tokens = walletTokens();
  int y = yTop;

  if (tokens.empty()) {
    s_tft->setFreeFont(nullptr);
    s_tft->setTextFont(2);
    s_tft->setTextDatum(TC_DATUM);
    s_tft->setTextColor(UI_C_TEXT_DIM, UI_C_BG);
    s_tft->drawString("(no SPL tokens yet)", SCR_W / 2, y + 8);
    return;
  }

  // Pin USDC to the top.
  int usdcIdx = -1;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].mint == USDC_MINT_B58) { usdcIdx = (int)i; break; }
  }
  auto drawOne = [&](const TokenHolding &t) -> bool {
    int ny = paintTokenRow(t, y, yBottom);
    if (ny < 0) {
      s_tft->setFreeFont(nullptr);
      s_tft->setTextFont(2);
      s_tft->setTextDatum(TC_DATUM);
      s_tft->setTextColor(UI_C_TEXT_DIM, UI_C_BG);
      s_tft->drawString("…more…", SCR_W / 2, y + 4);
      return false;
    }
    y = ny;
    return true;
  };
  if (usdcIdx >= 0 && !drawOne(tokens[usdcIdx])) return;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if ((int)i == usdcIdx) continue;
    if (!drawOne(tokens[i])) return;
  }
}

// ---------------------------------------------------------------------------
// QR overlay
// ---------------------------------------------------------------------------
static void paintQrOverlay() {
  s_tft->fillScreen(UI_C_BG);

  QRCode qr;
  constexpr uint8_t VERSION = 3;
  uint8_t data[qrcode_getBufferSize(VERSION)];
  qrcode_initText(&qr, data, VERSION, ECC_LOW, walletPubkey().c_str());

  const int modules = qr.size;          // 29 for version 3
  const int scale   = 6;                // 29 × 6 = 174 px
  const int qrPx    = modules * scale;
  const int qrX     = (SCR_W - qrPx) / 2;
  const int qrY     = 50;

  // White quiet-zone backing.
  s_tft->fillRect(qrX - scale, qrY - scale,
                  qrPx + 2 * scale, qrPx + 2 * scale, TFT_WHITE);
  for (int y = 0; y < modules; ++y) {
    for (int x = 0; x < modules; ++x) {
      if (qrcode_getModule(&qr, x, y)) {
        s_tft->fillRect(qrX + x * scale, qrY + y * scale,
                        scale, scale, TFT_BLACK);
      }
    }
  }

  String addr = walletPubkey();
  int addrY = qrY + qrPx + 12;
  s_tft->setFreeFont(nullptr);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(UI_C_TEXT, UI_C_BG);
  if (addr.length() <= 22) {
    s_tft->drawString(addr, SCR_W / 2, addrY);
  } else {
    int split = addr.length() / 2;
    s_tft->drawString(addr.substring(0, split), SCR_W / 2, addrY);
    s_tft->drawString(addr.substring(split),    SCR_W / 2, addrY + 16);
  }

  s_tft->setTextColor(UI_C_TEXT_DIM, UI_C_BG);
  s_tft->drawString("tap to close", SCR_W / 2, SCR_H - 16);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool walletScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

static void fullPaint() {
  s_tft->fillScreen(UI_C_BG);
  paintStatusBar();
  paintHeroCard();
  paintHoldingsHeader(HOLDINGS_Y);
  paintTokens(HOLDINGS_Y + 18, SCR_H - 4);

  s_lastSolShown   = walletSolBalance();
  s_lastPriceShown = priceSOLUSD();
  s_lastTokenCount = walletTokens().size();
}

// Callback for screenfxSlideIn — temporarily re-point s_tft at the sprite
// so our painters render off-screen.
static void paintIntoSprite(TFT_eSPI *sprite) {
  TFT_eSPI *real = s_tft;
  s_tft = sprite;
  fullPaint();
  s_tft = real;
}

void walletScreenOnEnter() {
  s_qrOverlay = false;
  s_wantClose = false;
  if (!screenfxSlideIn(s_tft, +SCR_H, paintIntoSprite)) {
    fullPaint();
  }
}

void walletScreenDraw() {
  if (!s_tft) return;
  if (s_qrOverlay) { paintQrOverlay(); return; }
  fullPaint();
}

void walletScreenTick() {
  if (!s_tft) return;

  uint32_t now = millis();
  if (now - s_lastTickMs < 200) return;    // ~5 Hz
  s_lastTickMs = now;

  if (s_qrOverlay) return;

  bool changed =
      (walletSolBalance()    != s_lastSolShown)   ||
      (priceSOLUSD()         != s_lastPriceShown) ||
      (walletTokens().size() != s_lastTokenCount);

  if (changed) { fullPaint(); return; }
  paintStatusBar();
}

void walletScreenHandleTap(int16_t x, int16_t y) {
  if (!s_tft) return;

  if (s_qrOverlay) {
    s_qrOverlay = false;
    fullPaint();
    return;
  }

  // X → home.
  if (x >= CLOSE_X && x < CLOSE_X + CLOSE_W &&
      y >= CLOSE_Y && y < CLOSE_Y + CLOSE_H) {
    s_wantClose = true;
    return;
  }
  // QR button → overlay.
  if (x >= QR_X && x < QR_X + QR_W &&
      y >= TOPBAR_Y && y < TOPBAR_Y + QR_H) {
    s_qrOverlay = true;
    paintQrOverlay();
    return;
  }
}

bool walletScreenConsumeClose() {
  if (!s_wantClose) return false;
  s_wantClose = false;
  return true;
}
