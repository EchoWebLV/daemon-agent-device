#include "walletscreen.h"
#include "wallet.h"
#include "price.h"

static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 16;

// Palette (RGB565)
static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_ACCENT    = 0x04BF;    // Daemon blue
static constexpr uint16_t C_ACCENT_HI = 0x07FF;    // cyan rim
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_DIM       = 0x7BEF;
static constexpr uint16_t C_GREEN     = 0x07E0;
static constexpr uint16_t C_DIVIDER   = 0x18E3;

static TFT_eSPI *s_tft = nullptr;

// We redraw the screen once on enter and again whenever the underlying
// wallet numbers change. This flag is reset each full paint so the tick
// function can cheaply repaint only the dynamic USD value.
static double   s_lastSolShown  = -1.0;
static double   s_lastPriceShown = -1.0;
static size_t   s_lastTokenCount = (size_t)-1;
static uint32_t s_lastTickMs     = 0;

bool walletScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

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

// ---------------------------------------------------------------------------
// Painters
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, STATUS_H, C_BG);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextFont(1);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(4, 4);
  s_tft->print("WALLET");

  // right: SOL price ticker (same as creature screen)
  String price = priceDisplayString();
  if (price.length() > 0) {
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_ACCENT_HI, C_BG);
    s_tft->drawString(price, SCR_W - 4, 4);
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

static void paintTokens(int16_t yTop, int16_t yBottom) {
  const auto &tokens = walletTokens();
  const int  lineH   = 18;
  int y = yTop;

  s_tft->setTextFont(2);
  if (tokens.empty()) {
    s_tft->setTextDatum(TL_DATUM);
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->drawString("(no SPL tokens)", 10, y + 2);
    return;
  }

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (y + lineH > yBottom) {
      // Didn't fit. Draw an ellipsis and stop.
      s_tft->setTextDatum(TL_DATUM);
      s_tft->setTextColor(C_DIM, C_BG);
      s_tft->drawString("...", 10, y);
      break;
    }
    const TokenHolding &t = tokens[i];
    String amount = withCommas(fmtAmount(t.amount));
    String symbol = (t.symbol.length() > 0) ? t.symbol
                                            : t.mint.substring(0, 4) + "…";

    // Left: amount (right-aligned at 140 px)
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_TEXT, C_BG);
    s_tft->drawString(amount, 140, y);

    // Right: symbol (left-aligned after 150 px)
    s_tft->setTextDatum(TL_DATUM);
    s_tft->setTextColor(C_ACCENT, C_BG);
    s_tft->setCursor(150, y);
    s_tft->print(symbol);

    y += lineH;
  }
}

static void paintFooter() {
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextFont(1);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString("swipe right  ->  DAEMON", SCR_W / 2, SCR_H - 10);
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
void walletScreenDraw() {
  if (!s_tft) return;
  s_tft->fillScreen(C_BG);
  paintStatusBar();

  const int16_t HEADER_Y = 26;
  paintHeader(HEADER_Y);

  const int16_t DIV_Y = HEADER_Y + 120;   // ~y=146
  paintDivider(DIV_Y, "HOLDINGS");

  paintTokens(DIV_Y + 24, SCR_H - 18);
  paintFooter();

  s_lastSolShown   = walletSolBalance();
  s_lastPriceShown = priceSOLUSD();
  s_lastTokenCount = walletTokens().size();
}

void walletScreenTick() {
  if (!s_tft) return;

  uint32_t now = millis();
  if (now - s_lastTickMs < 200) return;       // cheap dynamic repaint ~5 Hz
  s_lastTickMs = now;

  bool changed =
      (walletSolBalance()  != s_lastSolShown)   ||
      (priceSOLUSD()       != s_lastPriceShown) ||
      (walletTokens().size() != s_lastTokenCount);

  if (changed) {
    walletScreenDraw();
    return;
  }

  // Otherwise just keep the status-bar price ticker fresh (in case the
  // price updated without numbers otherwise changing).
  paintStatusBar();
}
