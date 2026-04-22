#include "walletscreen.h"
#include "wallet.h"
#include "price.h"
#include "base58.h"

#include <qrcode.h>

static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 28;

// Known mints that get special treatment in the holdings list.
static const char *USDC_MINT_B58 = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

// Palette (RGB565)
static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_ACCENT    = 0x04BF;    // Daemon blue
static constexpr uint16_t C_ACCENT_HI = 0x07FF;    // cyan rim
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_DIM       = 0x7BEF;
static constexpr uint16_t C_DIVIDER   = 0x18E3;

// Tap hit-rects (screen-space)
static constexpr int16_t QR_X  = 6,  QR_Y  = 2,  QR_W  = 28, QR_H = 24;
static constexpr int16_t CLOSE_X = SCR_W - 34, CLOSE_Y = 2, CLOSE_W = 28, CLOSE_H = 24;

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
// Helpers
// ---------------------------------------------------------------------------
static String truncAddr(const String &p) {
  if (p.length() < 10) return p;
  return p.substring(0, 6) + "…" + p.substring(p.length() - 6);
}

static String fmtAmount(double a) {
  char buf[32];
  if (a >= 10000)      snprintf(buf, sizeof(buf), "%.0f",  a);
  else if (a >= 100)   snprintf(buf, sizeof(buf), "%.1f",  a);
  else if (a >= 1)     snprintf(buf, sizeof(buf), "%.3f",  a);
  else                 snprintf(buf, sizeof(buf), "%.5f",  a);
  return String(buf);
}

// Insert thousand-separator commas into an integer string, e.g. "12450" ->
// "12,450". Operates on the part before the decimal point if any.
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

// Deterministic color from the first 3 bytes of the base58-decoded mint.
// Brightens the floor so every row gets a readable color, not black mud.
static uint16_t colorForMint(const String &mintB58) {
  auto bytes = base58Decode(mintB58);
  uint8_t r = 0x80, g = 0x80, b = 0x80;  // fallback neutral gray
  if (bytes.size() >= 3) {
    r = bytes[0]; g = bytes[1]; b = bytes[2];
    // Floor each channel so we never render a too-dark square.
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
// Top-bar icons. Drawn in fixed rects so tap hit-tests stay simple.
// ---------------------------------------------------------------------------
static void drawQrIcon(int16_t x, int16_t y) {
  // 3 position-detection markers arranged like a QR's corners. Each marker
  // is a 7x7 outer ring with a 3x3 solid inner square — just like a real
  // QR finder pattern, shrunk down.
  auto marker = [](TFT_eSPI *t, int16_t mx, int16_t my) {
    t->fillRect(mx, my, 7, 7, C_ACCENT_HI);
    t->fillRect(mx + 1, my + 1, 5, 5, C_BG);
    t->fillRect(mx + 2, my + 2, 3, 3, C_ACCENT_HI);
  };
  // Clear the button rect.
  s_tft->fillRect(x, y, QR_W, QR_H, C_BG);
  // Three markers (top-left, top-right, bottom-left) inside a 16x16 region
  // offset by (6, 4) within the 28x24 button rect.
  const int16_t ox = x + 6, oy = y + 4;
  marker(s_tft, ox,       oy);
  marker(s_tft, ox + 9,   oy);
  marker(s_tft, ox,       oy + 9);
}

static void drawCloseIcon(int16_t x, int16_t y) {
  s_tft->fillRect(x, y, CLOSE_W, CLOSE_H, C_BG);
  // Two crossed diagonals, 3 px stroke each, across a 16x16 region inside
  // the 28x24 button rect.
  const int16_t ox = x + 6, oy = y + 4;
  for (int i = -1; i <= 1; ++i) {
    s_tft->drawLine(ox + 0 + i, oy + 0,     ox + 15 + i, oy + 15,     C_TEXT);
    s_tft->drawLine(ox + 15 + i, oy + 0,    ox + 0 + i,  oy + 15,     C_TEXT);
  }
}

// ---------------------------------------------------------------------------
// Painters
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, STATUS_H, C_BG);
  drawQrIcon(QR_X, QR_Y);
  drawCloseIcon(CLOSE_X, CLOSE_Y);

  // Center: title + price ticker
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextFont(1);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->drawString("WALLET", SCR_W / 2, 8);
  String price = priceDisplayString();
  if (price.length() > 0) {
    s_tft->setTextColor(C_ACCENT_HI, C_BG);
    s_tft->drawString(price, SCR_W / 2, 20);
  }
}

static void paintHeader(int16_t y) {
  // Address
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextFont(2);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString(truncAddr(walletPubkey()), SCR_W / 2, y);

  // Big SOL balance
  double sol = walletSolBalance();
  char solBuf[32];
  snprintf(solBuf, sizeof(solBuf), "%.4f", sol);
  s_tft->setTextFont(6);                       // big 7-seg style digits
  s_tft->setTextColor(C_ACCENT_HI, C_BG);
  s_tft->drawString(solBuf, SCR_W / 2, y + 38);

  // "SOL" suffix label under the big number
  s_tft->setTextFont(2);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->drawString("SOL", SCR_W / 2, y + 74);

  // USD value
  double p = priceSOLUSD();
  char usdBuf[32];
  if (p > 0) snprintf(usdBuf, sizeof(usdBuf), "~ $%.2f", sol * p);
  else       snprintf(usdBuf, sizeof(usdBuf), "~ $ --");
  s_tft->setTextColor(C_TEXT, C_BG);
  s_tft->drawString(usdBuf, SCR_W / 2, y + 94);
}

static void paintDivider(int16_t y, const char *label) {
  s_tft->fillRect(0, y, SCR_W, 1, C_DIVIDER);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextFont(2);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(6, y + 4);
  s_tft->print(label);
}

// Draw a single token row. Returns the y-coord to use for the next row,
// or -1 if we ran out of vertical space and caller should render the
// "..." overflow marker instead.
static int paintTokenRow(const TokenHolding &t, int16_t y, int16_t yBottom) {
  const int lineH = 18;
  if (y + lineH > yBottom) return -1;

  String amount = withCommas(fmtAmount(t.amount));
  String symbol = (t.symbol.length() > 0) ? t.symbol
                                          : t.mint.substring(0, 4) + "…";

  // Left: 8x8 mint-hash colored square at (8, y+4).
  s_tft->fillRect(8, y + 4, 8, 8, colorForMint(t.mint));

  // Amount (right-aligned at x=150)
  s_tft->setTextDatum(TR_DATUM);
  s_tft->setTextColor(C_TEXT, C_BG);
  s_tft->setTextFont(2);
  s_tft->drawString(amount, 150, y);

  // Symbol (left-aligned after x=158)
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(158, y);
  s_tft->print(symbol);

  return y + lineH;
}

static void paintTokens(int16_t yTop, int16_t yBottom) {
  const auto &tokens = walletTokens();
  int y = yTop;

  s_tft->setTextFont(2);
  if (tokens.empty()) {
    s_tft->setTextDatum(TL_DATUM);
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->drawString("(no SPL tokens)", 10, y + 2);
    return;
  }

  // USDC first (if present), then everything else in existing order. That
  // way the balance the user actually spends on x402 is always visible at
  // the top of the list.
  int usdcIdx = -1;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].mint == USDC_MINT_B58) { usdcIdx = (int)i; break; }
  }
  if (usdcIdx >= 0) {
    int ny = paintTokenRow(tokens[usdcIdx], y, yBottom);
    if (ny < 0) {
      s_tft->setTextDatum(TL_DATUM);
      s_tft->setTextColor(C_DIM, C_BG);
      s_tft->drawString("...", 10, y);
      return;
    }
    y = ny;
  }
  for (size_t i = 0; i < tokens.size(); ++i) {
    if ((int)i == usdcIdx) continue;
    int ny = paintTokenRow(tokens[i], y, yBottom);
    if (ny < 0) {
      s_tft->setTextDatum(TL_DATUM);
      s_tft->setTextColor(C_DIM, C_BG);
      s_tft->drawString("...", 10, y);
      return;
    }
    y = ny;
  }
}

// ---------------------------------------------------------------------------
// QR overlay — drawn on top of the existing wallet paint. Tap anywhere to
// dismiss (handled in walletScreenHandleTap).
// ---------------------------------------------------------------------------
static void paintQrOverlay() {
  // Full wipe so no wallet pixels bleed through under the QR.
  s_tft->fillScreen(C_BG);

  // Render the QR. Version 3 @ ECC L is plenty for a 44-char pubkey.
  QRCode qr;
  constexpr uint8_t VERSION = 3;
  uint8_t data[qrcode_getBufferSize(VERSION)];
  qrcode_initText(&qr, data, VERSION, ECC_LOW, walletPubkey().c_str());

  const int modules = qr.size;          // 29 for version 3
  const int scale   = 6;                // 29 * 6 = 174 px
  const int qrPx    = modules * scale;
  const int qrX     = (SCR_W - qrPx) / 2;
  const int qrY     = 50;

  // White backing so quiet-zone stays white under the QR modules.
  s_tft->fillRect(qrX - scale, qrY - scale,
                  qrPx + 2 * scale, qrPx + 2 * scale, TFT_WHITE);
  for (int y = 0; y < modules; ++y) {
    for (int x = 0; x < modules; ++x) {
      if (qrcode_getModule(&qr, x, y)) {
        s_tft->fillRect(qrX + x * scale, qrY + y * scale, scale, scale, TFT_BLACK);
      }
    }
  }

  // Address as two lines of 22 chars under the QR.
  String addr = walletPubkey();
  int addrY = qrY + qrPx + 10;
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(C_TEXT, C_BG);
  if (addr.length() <= 22) {
    s_tft->drawString(addr, SCR_W / 2, addrY);
  } else {
    int split = addr.length() / 2;
    s_tft->drawString(addr.substring(0, split),     SCR_W / 2, addrY);
    s_tft->drawString(addr.substring(split),        SCR_W / 2, addrY + 16);
  }

  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString("tap to close", SCR_W / 2, SCR_H - 16);
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
bool walletScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

static void fullPaint() {
  s_tft->fillScreen(C_BG);
  paintStatusBar();

  const int16_t HEADER_Y = 40;
  paintHeader(HEADER_Y);

  const int16_t DIV_Y = HEADER_Y + 120;
  paintDivider(DIV_Y, "HOLDINGS");

  paintTokens(DIV_Y + 24, SCR_H - 6);

  s_lastSolShown   = walletSolBalance();
  s_lastPriceShown = priceSOLUSD();
  s_lastTokenCount = walletTokens().size();
}

// Slide-up reveal. We render the wallet once to an off-screen sprite in
// PSRAM, then blit it onto the display at progressively smaller y offsets
// so it appears to push up from the bottom. Falls back to an instant full
// paint if the sprite allocation fails.
static void slideUpReveal() {
  if (!s_tft) return;

  TFT_eSprite sprite(s_tft);
  sprite.setColorDepth(16);
  // Prefer PSRAM so we don't evict the audio decoder's big input buffer.
  sprite.setAttribute(PSRAM_ENABLE, true);
  void *ok = sprite.createSprite(SCR_W, SCR_H);
  if (!ok) {
    Serial.println("walletscreen: sprite alloc failed, doing instant draw");
    fullPaint();
    return;
  }

  // Paint the entire wallet into the sprite by temporarily swapping the
  // render target. All painters use `s_tft`, so we just repoint it for
  // the duration of this render, then restore.
  TFT_eSPI *realTft = s_tft;
  s_tft = &sprite;
  fullPaint();
  s_tft = realTft;

  // Black the screen so old creature pixels don't peek through the bands
  // between successive sprite positions.
  s_tft->fillScreen(C_BG);

  // 14 frames, easing in with a simple quadratic curve so the sheet
  // decelerates as it snaps into place.
  const int FRAMES = 14;
  for (int i = 1; i <= FRAMES; ++i) {
    float t = (float)i / (float)FRAMES;
    float eased = 1.0f - (1.0f - t) * (1.0f - t);
    int y = (int)((1.0f - eased) * SCR_H);
    sprite.pushSprite(0, y);
    // Wipe the strip above the sprite so no stale pixels remain there.
    if (y > 0) s_tft->fillRect(0, 0, SCR_W, y, C_BG);
    delay(15);
  }
  // Final frame at y=0.
  sprite.pushSprite(0, 0);
  sprite.deleteSprite();

  // Mark "already painted" so tick() doesn't immediately repaint and
  // flash over the animation we just finished.
  s_lastSolShown   = walletSolBalance();
  s_lastPriceShown = priceSOLUSD();
  s_lastTokenCount = walletTokens().size();
}

void walletScreenOnEnter() {
  s_qrOverlay = false;
  s_wantClose = false;
  slideUpReveal();
}

void walletScreenDraw() {
  if (!s_tft) return;
  if (s_qrOverlay) { paintQrOverlay(); return; }
  fullPaint();
}

void walletScreenTick() {
  if (!s_tft) return;

  uint32_t now = millis();
  if (now - s_lastTickMs < 200) return;       // cheap dynamic repaint ~5 Hz
  s_lastTickMs = now;

  if (s_qrOverlay) return;   // static; only redrawn on enter/dismiss

  bool changed =
      (walletSolBalance()  != s_lastSolShown)   ||
      (priceSOLUSD()       != s_lastPriceShown) ||
      (walletTokens().size() != s_lastTokenCount);

  if (changed) {
    fullPaint();
    return;
  }

  // Otherwise just keep the status-bar price ticker fresh.
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

  // Hit-test the top-bar buttons.
  if (x >= CLOSE_X && x < CLOSE_X + CLOSE_W &&
      y >= CLOSE_Y && y < CLOSE_Y + CLOSE_H) {
    s_wantClose = true;
    return;
  }
  if (x >= QR_X && x < QR_X + QR_W &&
      y >= QR_Y && y < QR_Y + QR_H) {
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
