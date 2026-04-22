#include "walletscreen.h"
#include "wallet.h"
#include "price.h"
#include "base58.h"
#include "screenfx.h"

#include <qrcode.h>

static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 30;

// Known mints that get special treatment in the holdings list.
static const char *USDC_MINT_B58 = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

// Palette (RGB565). Keep in sync with settingsscreen so the top bars feel
// like they belong to the same OS.
static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_PANEL     = 0x1082;   // dark slate pill fill
static constexpr uint16_t C_ACCENT    = 0x0AFF;   // Daemon blue
static constexpr uint16_t C_ACCENT_HI = 0x07FF;   // cyan rim
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_DIM       = 0x7BEF;
static constexpr uint16_t C_DIMMER    = 0x39C7;
static constexpr uint16_t C_DIVIDER   = 0x18E3;

// Top-bar button geometry. Both buttons are 28×24 and sit flush against
// the edges of a 30-px status strip so the hit-rects are easy to land on.
static constexpr int16_t TOPBAR_Y    = 3;
static constexpr int16_t QR_X        = 6;
static constexpr int16_t QR_W        = 28;
static constexpr int16_t QR_H        = 24;
static constexpr int16_t CLOSE_X     = SCR_W - SCREENFX_X_BTN_W - 6;
static constexpr int16_t CLOSE_Y     = TOPBAR_Y;
static constexpr int16_t CLOSE_W     = SCREENFX_X_BTN_W;
static constexpr int16_t CLOSE_H     = SCREENFX_X_BTN_H;

// "Render target" pointer. Normally points at the real TFT, but is swapped
// out for a sprite during the slide-in animation so every painter reuses
// the same drawing code regardless of target.
static TFT_eSPI *s_tft = nullptr;

// ---------------------------------------------------------------------------
// State the tick loop watches to decide when to repaint.
// ---------------------------------------------------------------------------
static double   s_lastSolShown   = -1.0;
static double   s_lastPriceShown = -1.0;
static size_t   s_lastTokenCount = (size_t)-1;
static uint32_t s_lastTickMs     = 0;

// Latched intents consumed by main.cpp.
static bool     s_wantClose      = false;

// When true, a full-screen QR code is painted on top of the wallet and
// every tap closes the overlay instead of hitting wallet controls.
static bool     s_qrOverlay      = false;

// ---------------------------------------------------------------------------
// Small formatting helpers
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

// Deterministic per-mint color. We floor each channel at 0x60 so the
// result always has enough luminance to read dark text on top.
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
// QR button — three position-detection markers, Phantom-style miniature
// ---------------------------------------------------------------------------
static void drawQrIcon(int16_t x, int16_t y) {
  auto marker = [](TFT_eSPI *t, int16_t mx, int16_t my) {
    t->fillRect(mx,     my,     7, 7, C_ACCENT);
    t->fillRect(mx + 1, my + 1, 5, 5, C_BG);
    t->fillRect(mx + 2, my + 2, 3, 3, C_ACCENT);
  };
  s_tft->fillRect(x, y, QR_W, QR_H, C_BG);
  s_tft->drawRoundRect(x, y, QR_W, QR_H, 5, C_ACCENT);
  const int16_t ox = x + 5, oy = y + 3;
  marker(s_tft, ox,     oy);
  marker(s_tft, ox + 9, oy);
  marker(s_tft, ox,     oy + 9);
  // one extra module at bottom-right so it doesn't look lopsided
  s_tft->fillRect(ox + 10, oy + 10, 3, 3, C_ACCENT);
  s_tft->fillRect(ox + 14, oy + 10, 3, 3, C_ACCENT);
  s_tft->fillRect(ox + 10, oy + 14, 3, 3, C_ACCENT);
}

// ---------------------------------------------------------------------------
// Painters
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, STATUS_H, C_BG);

  drawQrIcon(QR_X, TOPBAR_Y);
  screenfxDrawXButton(s_tft, CLOSE_X, CLOSE_Y, C_ACCENT, C_BG);

  // Address "pill" in the middle of the bar — reads as a Phantom-style
  // copy chip. We don't actually copy anything on tap (tap opens QR via
  // the dedicated QR button), but visually it anchors the bar.
  const int16_t pillX = QR_X + QR_W + 6;
  const int16_t pillW = CLOSE_X - pillX - 6;
  const int16_t pillY = TOPBAR_Y + 2;
  const int16_t pillH = 20;
  s_tft->fillRoundRect(pillX, pillY, pillW, pillH, pillH / 2, C_PANEL);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(C_TEXT, C_PANEL);
  s_tft->drawString(truncAddr(walletPubkey()),
                    pillX + pillW / 2,
                    pillY + pillH / 2 + 1);
}

static void paintHeader(int16_t y) {
  // Big SOL balance, centered. Font 7 draws the slim 7-segment digits at
  // ~48 px tall — perfect for a Phantom-style hero number.
  double sol = walletSolBalance();
  char solBuf[32];
  snprintf(solBuf, sizeof(solBuf), "%.3f", sol);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextFont(7);
  s_tft->setTextColor(C_TEXT, C_BG);
  s_tft->drawString(solBuf, SCR_W / 2, y + 22);

  // "SOL" label immediately under the big digits.
  s_tft->setTextFont(2);
  s_tft->setTextColor(C_ACCENT_HI, C_BG);
  s_tft->drawString("SOL", SCR_W / 2, y + 54);

  // USD valuation — the dim "≈ $86.22" subline.
  double p = priceSOLUSD();
  char   usdBuf[32];
  if (p > 0) snprintf(usdBuf, sizeof(usdBuf), "≈ $%.2f", sol * p);
  else       snprintf(usdBuf, sizeof(usdBuf), "≈ $ —");
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString(usdBuf, SCR_W / 2, y + 74);
}

// "HOLDINGS" section header: a thin divider bar with the label and a
// right-aligned token count.
static void paintHoldingsHeader(int16_t y) {
  s_tft->fillRect(0, y, SCR_W, 1, C_DIVIDER);

  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(10, y + 6);
  s_tft->print("HOLDINGS");

  const auto &tokens = walletTokens();
  char cbuf[16];
  // +1 because SOL sits above as a separate hero, but the row count
  // here counts SPL tokens only.
  snprintf(cbuf, sizeof(cbuf), "%u tokens", (unsigned)tokens.size());
  s_tft->setTextDatum(TR_DATUM);
  s_tft->setTextColor(C_DIMMER, C_BG);
  s_tft->drawString(cbuf, SCR_W - 10, y + 6);
}

// Single token row — circular color chip with the symbol's first letter,
// symbol name, and right-aligned amount. Returns the next-row y, or -1
// if we ran out of vertical space.
static int paintTokenRow(const TokenHolding &t,
                         int16_t              y,
                         int16_t              yBottom) {
  constexpr int ROW_H = 30;
  if (y + ROW_H > yBottom) return -1;

  String symbol = (t.symbol.length() > 0) ? t.symbol
                                          : t.mint.substring(0, 4) + "…";
  String amount = withCommas(fmtAmount(t.amount));

  // Colored circle chip on the left with the symbol's first letter.
  const int16_t chipR = 11;
  const int16_t chipX = 10 + chipR;
  const int16_t chipY = y + ROW_H / 2;
  const uint16_t chipC = colorForMint(t.mint);
  s_tft->fillCircle(chipX, chipY, chipR, chipC);

  // Letter inside chip. We tint the letter dark so it reads on the
  // mint-color background, which we already floored at 0x60.
  s_tft->setTextFont(2);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(0x0000, chipC);
  char letter[2] = { (char)toupper(symbol[0]), 0 };
  s_tft->drawString(letter, chipX, chipY + 1);

  // Symbol (bright) just to the right of the chip.
  s_tft->setTextFont(2);
  s_tft->setTextDatum(ML_DATUM);
  s_tft->setTextColor(C_TEXT, C_BG);
  s_tft->drawString(symbol, chipX + chipR + 8, chipY);

  // Amount right-aligned.
  s_tft->setTextDatum(MR_DATUM);
  s_tft->setTextColor(C_ACCENT_HI, C_BG);
  s_tft->drawString(amount, SCR_W - 10, chipY);

  return y + ROW_H;
}

static void paintTokens(int16_t yTop, int16_t yBottom) {
  const auto &tokens = walletTokens();
  int y = yTop;

  if (tokens.empty()) {
    s_tft->setTextFont(2);
    s_tft->setTextDatum(TC_DATUM);
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->drawString("(no SPL tokens yet)", SCR_W / 2, y + 8);
    return;
  }

  // USDC pinned first so the x402 spending balance is always visible.
  int usdcIdx = -1;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].mint == USDC_MINT_B58) { usdcIdx = (int)i; break; }
  }
  auto drawOne = [&](const TokenHolding &t) -> bool {
    int ny = paintTokenRow(t, y, yBottom);
    if (ny < 0) {
      s_tft->setTextFont(2);
      s_tft->setTextDatum(TC_DATUM);
      s_tft->setTextColor(C_DIM, C_BG);
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
// QR overlay — drawn on top of the existing wallet paint. Tap anywhere to
// dismiss (handled in walletScreenHandleTap).
// ---------------------------------------------------------------------------
static void paintQrOverlay() {
  s_tft->fillScreen(C_BG);

  QRCode qr;
  constexpr uint8_t VERSION = 3;
  uint8_t data[qrcode_getBufferSize(VERSION)];
  qrcode_initText(&qr, data, VERSION, ECC_LOW, walletPubkey().c_str());

  const int modules = qr.size;          // 29 for version 3
  const int scale   = 6;                // 29 × 6 = 174 px
  const int qrPx    = modules * scale;
  const int qrX     = (SCR_W - qrPx) / 2;
  const int qrY     = 50;

  // White backing so the QR quiet zone is, in fact, white.
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
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(C_TEXT, C_BG);
  if (addr.length() <= 22) {
    s_tft->drawString(addr, SCR_W / 2, addrY);
  } else {
    int split = addr.length() / 2;
    s_tft->drawString(addr.substring(0, split), SCR_W / 2, addrY);
    s_tft->drawString(addr.substring(split),    SCR_W / 2, addrY + 16);
  }

  s_tft->setTextColor(C_DIM, C_BG);
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
  s_tft->fillScreen(C_BG);
  paintStatusBar();

  const int16_t HEADER_Y = 36;
  paintHeader(HEADER_Y);

  const int16_t DIV_Y = 140;
  paintHoldingsHeader(DIV_Y);

  paintTokens(DIV_Y + 26, SCR_H - 4);

  s_lastSolShown   = walletSolBalance();
  s_lastPriceShown = priceSOLUSD();
  s_lastTokenCount = walletTokens().size();
}

// Callback for screenfxSlideIn — temporarily re-points `s_tft` to the
// sprite so every painter renders into the off-screen buffer instead of
// to the display, then restores.
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
    // PSRAM exhausted — no animation, just paint directly.
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
  if (now - s_lastTickMs < 200) return;   // ~5 Hz dynamic repaint cadence
  s_lastTickMs = now;

  if (s_qrOverlay) return;                // static; only redrawn on toggle

  bool changed =
      (walletSolBalance()    != s_lastSolShown)   ||
      (priceSOLUSD()         != s_lastPriceShown) ||
      (walletTokens().size() != s_lastTokenCount);

  if (changed) { fullPaint(); return; }

  // Otherwise keep the address pill fresh — cheap, only the top bar.
  paintStatusBar();
}

void walletScreenHandleTap(int16_t x, int16_t y) {
  if (!s_tft) return;

  if (s_qrOverlay) {
    // Any tap inside the overlay dismisses it.
    s_qrOverlay = false;
    fullPaint();
    return;
  }

  // X close button — takes us back to the creature.
  if (x >= CLOSE_X && x < CLOSE_X + CLOSE_W &&
      y >= CLOSE_Y && y < CLOSE_Y + CLOSE_H) {
    s_wantClose = true;
    return;
  }
  // QR button — open full-screen QR overlay.
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
