#include "walletscreen.h"
#include "wallet.h"
#include "price.h"
#include "statusicons.h"

#include <qrcode.h>

static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 16;

// Palette (RGB565) — tuned to feel like Phantom's dark theme: deep navy
// backgrounds, lavender-blue accent, soft lilac dim text.
static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_CARD      = 0x0861;   // subtle navy card
static constexpr uint16_t C_CARD_HI   = 0x10A3;   // slightly lifted navy
static constexpr uint16_t C_ACCENT    = 0x6B9F;   // Phantom-ish lavender
static constexpr uint16_t C_ACCENT_HI = 0x9CBF;   // brighter lavender
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_DIM       = 0x9493;
static constexpr uint16_t C_QR_BG     = 0xFFFF;   // QR background (white)
static constexpr uint16_t C_QR_FG     = 0x0000;   // QR ink (black)
static constexpr uint16_t C_DIVIDER   = 0x2124;

static TFT_eSPI *s_tft = nullptr;

// Cached dynamic values — the dense address+QR header is static once drawn,
// so the tick loop only repaints the balance and holdings when they change.
static double   s_lastSolShown   = -1.0;
static double   s_lastPriceShown = -1.0;
static size_t   s_lastTokenCount = (size_t)-1;
static uint32_t s_lastTickMs     = 0;
static String   s_qrAddrDrawn;                    // whose QR is on screen

// Layout anchors — computed top-down so everything is easy to shuffle.
static constexpr int16_t GAP_TOP        = STATUS_H + 4;

static constexpr int16_t ADDR_PILL_Y    = GAP_TOP;
static constexpr int16_t ADDR_PILL_H    = 22;

static constexpr int16_t QR_MODULE_PX   = 3;      // version-4 QR = 33 modules
static constexpr int16_t QR_SIZE_PX     = QR_MODULE_PX * 33;   // 99
static constexpr int16_t QR_PAD         = 8;      // white quiet-zone margin
static constexpr int16_t QR_BOX_SIZE    = QR_SIZE_PX + QR_PAD * 2; // 115
static constexpr int16_t QR_BOX_X       = (SCR_W - QR_BOX_SIZE) / 2;
static constexpr int16_t QR_BOX_Y       = ADDR_PILL_Y + ADDR_PILL_H + 6;  // ~48

static constexpr int16_t BAL_Y          = QR_BOX_Y + QR_BOX_SIZE + 10;    // ~173
static constexpr int16_t BAL_H          = 44;

static constexpr int16_t HOLD_DIV_Y     = BAL_Y + BAL_H + 6;              // ~223
static constexpr int16_t HOLD_LIST_Y    = HOLD_DIV_Y + 16;                // ~239
static constexpr int16_t FOOTER_Y       = SCR_H - 14;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
bool walletScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

static String truncAddr(const String &p) {
  if (p.length() < 14) return p;
  return p.substring(0, 5) + "…" + p.substring(p.length() - 5);
}

static String fmtAmount(double a) {
  char buf[32];
  if (a >= 10000)      snprintf(buf, sizeof(buf), "%.0f",  a);
  else if (a >= 100)   snprintf(buf, sizeof(buf), "%.1f",  a);
  else if (a >= 1)     snprintf(buf, sizeof(buf), "%.3f",  a);
  else                 snprintf(buf, sizeof(buf), "%.5f",  a);
  return String(buf);
}

// Insert thousand-separator commas into an integer string.
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

// ---------------------------------------------------------------------------
// Status bar (same layout as the other screens — title left, price right,
// feature indicators centred).
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, STATUS_H, C_BG);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextFont(1);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(4, 4);
  s_tft->print("WALLET");

  String price = priceDisplayString();
  if (price.length() > 0) {
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_ACCENT_HI, C_BG);
    s_tft->drawString(price, SCR_W - 4, 4);
  }

  statusIconsDraw(s_tft, SCR_W / 2, STATUS_H / 2, C_ACCENT_HI, C_BG);
}

// ---------------------------------------------------------------------------
// Address pill — rounded card with the truncated pubkey centred inside.
// ---------------------------------------------------------------------------
static void paintAddressPill() {
  const int16_t x = 20;
  const int16_t y = ADDR_PILL_Y;
  const int16_t w = SCR_W - 40;
  const int16_t h = ADDR_PILL_H;

  s_tft->fillRect(0, y, SCR_W, h, C_BG);          // clear row
  s_tft->fillRoundRect(x, y, w, h, h / 2, C_CARD);
  s_tft->drawRoundRect(x, y, w, h, h / 2, C_ACCENT);

  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextFont(2);
  s_tft->setTextColor(C_TEXT, C_CARD);
  s_tft->drawString(truncAddr(walletPubkey()), SCR_W / 2, y + h / 2);
}

// ---------------------------------------------------------------------------
// QR code of the receive address. Drawn once per address change so we don't
// pay the ~80 ms encode cost on every tick.
// ---------------------------------------------------------------------------
static void paintQr(bool force) {
  String addr = walletPubkey();
  if (!force && addr == s_qrAddrDrawn) return;
  s_qrAddrDrawn = addr;

  // White quiet-zone background (larger than the code by QR_PAD on each
  // side). Rounded corners + lavender frame give it a Phantom-card look.
  s_tft->fillRoundRect(QR_BOX_X - 3, QR_BOX_Y - 3,
                       QR_BOX_SIZE + 6, QR_BOX_SIZE + 6, 8, C_ACCENT);
  s_tft->fillRoundRect(QR_BOX_X, QR_BOX_Y,
                       QR_BOX_SIZE, QR_BOX_SIZE, 6, C_QR_BG);

  if (addr.length() == 0) {
    s_tft->setTextDatum(MC_DATUM);
    s_tft->setTextFont(2);
    s_tft->setTextColor(C_DIM, C_QR_BG);
    s_tft->drawString("no wallet", QR_BOX_X + QR_BOX_SIZE / 2,
                                   QR_BOX_Y + QR_BOX_SIZE / 2);
    return;
  }

  // Version-4 QR (33 modules) comfortably fits a 44-char base58 pubkey
  // with low error correction. Buffer is sized by the library helper.
  const uint8_t version = 4;
  QRCode qr;
  uint8_t buf[qrcode_getBufferSize(version)];
  int8_t rc = qrcode_initText(&qr, buf, version, ECC_LOW, addr.c_str());
  if (rc != 0) {
    s_tft->setTextDatum(MC_DATUM);
    s_tft->setTextFont(2);
    s_tft->setTextColor(C_DIM, C_QR_BG);
    s_tft->drawString("qr error", QR_BOX_X + QR_BOX_SIZE / 2,
                                  QR_BOX_Y + QR_BOX_SIZE / 2);
    return;
  }

  const int16_t x0 = QR_BOX_X + QR_PAD;
  const int16_t y0 = QR_BOX_Y + QR_PAD;
  for (uint8_t j = 0; j < qr.size; ++j) {
    for (uint8_t i = 0; i < qr.size; ++i) {
      if (qrcode_getModule(&qr, i, j)) {
        s_tft->fillRect(x0 + i * QR_MODULE_PX,
                        y0 + j * QR_MODULE_PX,
                        QR_MODULE_PX, QR_MODULE_PX, C_QR_FG);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Balance card — single rounded rect with the big SOL number on top and the
// USD value underneath. Hides when the wallet has zero balance.
// ---------------------------------------------------------------------------
static void paintBalanceCard() {
  const int16_t x = 20;
  const int16_t y = BAL_Y;
  const int16_t w = SCR_W - 40;
  const int16_t h = BAL_H;

  s_tft->fillRect(0, y, SCR_W, h, C_BG);
  s_tft->fillRoundRect(x, y, w, h, 10, C_CARD);
  s_tft->drawRoundRect(x, y, w, h, 10, C_DIVIDER);

  double sol = walletSolBalance();
  char solBuf[32];
  snprintf(solBuf, sizeof(solBuf), "%.4f SOL", sol);

  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextFont(4);                          // bigger digits
  s_tft->setTextColor(C_TEXT, C_CARD);
  s_tft->drawString(solBuf, SCR_W / 2, y + 16);

  double p = priceSOLUSD();
  char usdBuf[32];
  if (p > 0) snprintf(usdBuf, sizeof(usdBuf), "~ $%.2f", sol * p);
  else       snprintf(usdBuf, sizeof(usdBuf), "~ $ --");
  s_tft->setTextFont(2);
  s_tft->setTextColor(C_DIM, C_CARD);
  s_tft->drawString(usdBuf, SCR_W / 2, y + h - 12);
}

// ---------------------------------------------------------------------------
// Holdings list — compact rows with amount + symbol, styled like Phantom's
// token cards. We only have room for ~2 rows on this layout; extras are
// summarised with an "…and N more" footer.
// ---------------------------------------------------------------------------
static void paintHoldings() {
  // Header line
  s_tft->fillRect(0, HOLD_DIV_Y, SCR_W, SCR_H - FOOTER_Y - HOLD_DIV_Y, C_BG);
  s_tft->drawFastHLine(20, HOLD_DIV_Y + 4, SCR_W - 40, C_DIVIDER);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextFont(1);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString("HOLDINGS", SCR_W / 2, HOLD_DIV_Y - 4);

  const auto &tokens = walletTokens();

  const int16_t rowH      = 22;
  const int16_t rowX      = 20;
  const int16_t rowW      = SCR_W - 40;
  const int   maxRows     = 2;
  const int   available   = FOOTER_Y - HOLD_LIST_Y;
  const int   rowsToDraw  = min((int)tokens.size(), maxRows);

  if (tokens.empty()) {
    s_tft->setTextDatum(TC_DATUM);
    s_tft->setTextFont(2);
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->drawString("(no SPL tokens)", SCR_W / 2, HOLD_LIST_Y + 4);
    return;
  }

  (void)available;
  for (int i = 0; i < rowsToDraw; ++i) {
    int16_t y = HOLD_LIST_Y + i * (rowH + 4);
    s_tft->fillRoundRect(rowX, y, rowW, rowH, 8, C_CARD);

    const TokenHolding &t = tokens[i];
    String amount = withCommas(fmtAmount(t.amount));
    String symbol = (t.symbol.length() > 0) ? t.symbol
                                            : t.mint.substring(0, 4) + "…";

    // Small colored dot on the left acts as a placeholder token icon.
    s_tft->fillCircle(rowX + 14, y + rowH / 2, 6, C_ACCENT);

    s_tft->setTextFont(2);
    s_tft->setTextDatum(ML_DATUM);
    s_tft->setTextColor(C_TEXT, C_CARD);
    s_tft->drawString(symbol, rowX + 28, y + rowH / 2);

    s_tft->setTextDatum(MR_DATUM);
    s_tft->setTextColor(C_ACCENT_HI, C_CARD);
    s_tft->drawString(amount, rowX + rowW - 10, y + rowH / 2);
  }

  if ((int)tokens.size() > rowsToDraw) {
    int16_t y = HOLD_LIST_Y + rowsToDraw * (rowH + 4) + 2;
    char buf[32];
    snprintf(buf, sizeof(buf), "+ %d more",
             (int)tokens.size() - rowsToDraw);
    s_tft->setTextDatum(TC_DATUM);
    s_tft->setTextFont(1);
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->drawString(buf, SCR_W / 2, y);
  }
}

static void paintFooter() {
  s_tft->fillRect(0, FOOTER_Y - 2, SCR_W, SCR_H - FOOTER_Y + 2, C_BG);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextFont(1);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString("swipe down to close", SCR_W / 2, FOOTER_Y);
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
void walletScreenDraw() {
  if (!s_tft) return;
  s_tft->fillScreen(C_BG);
  statusIconsResetCache();
  s_qrAddrDrawn = "";     // force QR repaint

  paintStatusBar();
  paintAddressPill();
  paintQr(true);
  paintBalanceCard();
  paintHoldings();
  paintFooter();

  s_lastSolShown   = walletSolBalance();
  s_lastPriceShown = priceSOLUSD();
  s_lastTokenCount = walletTokenCount();
}

void walletScreenTick() {
  if (!s_tft) return;

  uint32_t now = millis();
  if (now - s_lastTickMs < 200) return;       // ~5 Hz dynamic repaint
  s_lastTickMs = now;

  double sol  = walletSolBalance();
  double px   = priceSOLUSD();
  size_t nTok = walletTokenCount();           // cheap size-only query

  bool balanceChanged  = (sol != s_lastSolShown) || (px != s_lastPriceShown);
  bool holdingsChanged = (nTok != s_lastTokenCount);

  if (balanceChanged) {
    paintBalanceCard();
    s_lastSolShown   = sol;
    s_lastPriceShown = px;
  }
  if (holdingsChanged) {
    paintHoldings();
    s_lastTokenCount = nTok;
  }

  // Always keep the status bar fresh (price ticker + indicator toggles)
  // and make sure the QR is up to date if the wallet key somehow changed.
  paintStatusBar();
  if (walletPubkey() != s_qrAddrDrawn) {
    paintAddressPill();
    paintQr(false);
  }
}
