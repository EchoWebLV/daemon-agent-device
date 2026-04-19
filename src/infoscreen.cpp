#include "infoscreen.h"
#include "price.h"
#include "statusicons.h"
#include "touch.h"
#include "wallet.h"

#include <WiFi.h>
#include <esp_system.h>

// ---------------------------------------------------------------------------
// Geometry / palette — matches the other dark-themed screens.
// ---------------------------------------------------------------------------
static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 16;

static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_ACCENT    = 0x0AFF;
static constexpr uint16_t C_ACCENT_HI = 0x07FF;
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_DIM       = 0x7BEF;
static constexpr uint16_t C_DIVIDER   = 0x18E3;

// Close "X" button — same geometry as the other sub-screens so muscle
// memory works from every view.
static constexpr int16_t CLOSE_W = 36;
static constexpr int16_t CLOSE_H = 24;
static constexpr int16_t CLOSE_X = SCR_W - CLOSE_W;
static constexpr int16_t CLOSE_Y = 0;

// Row layout.
static constexpr int16_t ROW_X0    = 10;
static constexpr int16_t ROW_X1    = SCR_W - 10;
static constexpr int16_t ROW_H     = 22;
static constexpr int16_t LIST_TOP  = 32;       // just under the status bar
static constexpr int16_t LIST_BOT  = SCR_H - 20;

static TFT_eSPI *s_tft = nullptr;
static bool     s_wantClose     = false;

// Cached "last drawn" values — only repaint rows whose text actually
// changed so we don't thrash the display at every tick.
static String s_lastIp;
static String s_lastSsid;
static String s_lastRssi;
static String s_lastHeap;
static String s_lastUptime;
static String s_lastPrice = "\x01";

// Press / release tap tracking — same pattern as the other screens.
static bool     s_pressed     = false;
static int16_t  s_pressX      = 0;
static int16_t  s_pressY      = 0;
static bool     s_pressMoved  = false;
static constexpr int16_t TAP_MOVE_BUDGET = 14;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool inRect(int16_t x, int16_t y,
                   int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static String humanBytes(uint64_t n) {
  char buf[24];
  if (n >= (1ULL << 20))       snprintf(buf, sizeof(buf), "%.1f MB", n / 1048576.0);
  else if (n >= (1ULL << 10))  snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
  else                         snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
  return String(buf);
}

static String uptimeString() {
  uint32_t s = millis() / 1000;
  uint32_t d = s / 86400;  s %= 86400;
  uint32_t h = s / 3600;   s %= 3600;
  uint32_t m = s / 60;     s %= 60;
  char buf[24];
  if (d > 0) snprintf(buf, sizeof(buf), "%ud %02u:%02u:%02u", (unsigned)d,
                      (unsigned)h, (unsigned)m, (unsigned)s);
  else       snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
                      (unsigned)h, (unsigned)m, (unsigned)s);
  return String(buf);
}

static String truncAddr(const String &p) {
  if (p.length() < 14) return p;
  return p.substring(0, 6) + "…" + p.substring(p.length() - 6);
}

// Values that can change while the screen is open.
static String currentIp()     { return (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("--"); }
static String currentSsid()   { return (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("(offline)"); }
static String currentRssi()   { return (WiFi.status() == WL_CONNECTED) ? (String(WiFi.RSSI()) + " dBm") : String("--"); }
static String currentHeap()   { return humanBytes(ESP.getFreeHeap()); }

// ---------------------------------------------------------------------------
// Painters
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, CLOSE_H, C_BG);
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(4, 4);
  s_tft->print("INFO");

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

// Paint a single key/value row at the given y-slot (0-based).
// Wipes the background so the value can be redrawn in place.
static void paintRow(int slot, const char *label, const String &value) {
  int16_t y = LIST_TOP + slot * ROW_H;
  s_tft->fillRect(0, y, SCR_W, ROW_H, C_BG);

  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(ROW_X0, y + 4);
  s_tft->print(label);

  s_tft->setTextDatum(TR_DATUM);
  s_tft->setTextColor(C_TEXT, C_BG);
  s_tft->drawString(value, ROW_X1, y + 4);

  s_tft->drawFastHLine(0, y + ROW_H - 1, SCR_W, C_DIVIDER);
}

static void paintFooter() {
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString("swipe down to close", SCR_W / 2, SCR_H - 12);
}

// Static rows = never change while this screen is open. Drawn once on
// enter. Dynamic rows get their own slot numbers below and are kept
// fresh by the tick loop.
enum : int {
  ROW_IP = 0,
  ROW_SSID,
  ROW_RSSI,
  ROW_MAC,
  ROW_FIRMWARE,
  ROW_SDK,
  ROW_CHIP,
  ROW_FLASH,
  ROW_PSRAM,
  ROW_HEAP,
  ROW_UPTIME,
  ROW_WALLET,
  ROW_COUNT,
};

static void paintAllRows() {
  s_lastIp     = currentIp();
  s_lastSsid   = currentSsid();
  s_lastRssi   = currentRssi();
  s_lastHeap   = currentHeap();
  s_lastUptime = uptimeString();

  paintRow(ROW_IP,       "IP",       s_lastIp);
  paintRow(ROW_SSID,     "SSID",     s_lastSsid);
  paintRow(ROW_RSSI,     "SIGNAL",   s_lastRssi);
  paintRow(ROW_MAC,      "MAC",      WiFi.macAddress());

  char fwBuf[40];
  snprintf(fwBuf, sizeof(fwBuf), "%s %s", __DATE__, __TIME__);
  paintRow(ROW_FIRMWARE, "FIRMWARE", String(fwBuf));

  paintRow(ROW_SDK,      "SDK",      String(ESP.getSdkVersion()));

  char chipBuf[32];
  snprintf(chipBuf, sizeof(chipBuf), "%s rev%u",
           ESP.getChipModel(), (unsigned)ESP.getChipRevision());
  paintRow(ROW_CHIP,     "CHIP",     String(chipBuf));

  paintRow(ROW_FLASH,    "FLASH",    humanBytes(ESP.getFlashChipSize()));
  paintRow(ROW_PSRAM,    "PSRAM",    humanBytes(ESP.getPsramSize()));
  paintRow(ROW_HEAP,     "FREE HEAP", s_lastHeap);
  paintRow(ROW_UPTIME,   "UPTIME",   s_lastUptime);

  String pk = walletPubkey();
  paintRow(ROW_WALLET,   "WALLET",
           pk.length() > 0 ? truncAddr(pk) : String("(not loaded)"));
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
    } else if (abs(x - s_pressX) > TAP_MOVE_BUDGET ||
               abs(y - s_pressY) > TAP_MOVE_BUDGET) {
      s_pressMoved = true;
    }
  } else if (s_pressed) {
    s_pressed = false;
    if (s_pressMoved) return;

    if (inRect(s_pressX, s_pressY, CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H)) {
      s_wantClose = true;
    }
  }
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
bool infoScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

void infoScreenDraw() {
  if (!s_tft) return;
  s_tft->fillScreen(C_BG);
  statusIconsResetCache();
  paintStatusBar();
  paintAllRows();
  paintFooter();
}

void infoScreenDrawTo(TFT_eSprite *target) {
  if (!target) return;
  TFT_eSPI *saved = s_tft;
  s_tft = target;
  infoScreenDraw();
  s_tft = saved;
  statusIconsResetCache();
}

void infoScreenTick() {
  if (!s_tft) return;
  handleInput();

  // Status bar ticks at whatever cadence the price updates.
  String price = priceDisplayString();
  if (price != s_lastPrice || statusIconsNeedRedraw()) {
    paintStatusBar();
  }

  // Refresh dynamic rows. IP/SSID/signal can change if the user switches
  // networks while viewing; heap + uptime march forward on every tick but
  // we only repaint when the displayed string actually changes (heap) or
  // once per second (uptime), so writes stay cheap.
  static uint32_t s_lastUptimeTick = 0;
  uint32_t now = millis();

  String ip = currentIp();
  if (ip != s_lastIp) {
    s_lastIp = ip;
    paintRow(ROW_IP, "IP", ip);
  }

  String ssid = currentSsid();
  if (ssid != s_lastSsid) {
    s_lastSsid = ssid;
    paintRow(ROW_SSID, "SSID", ssid);
  }

  String rssi = currentRssi();
  if (rssi != s_lastRssi) {
    s_lastRssi = rssi;
    paintRow(ROW_RSSI, "SIGNAL", rssi);
  }

  String heap = currentHeap();
  if (heap != s_lastHeap) {
    s_lastHeap = heap;
    paintRow(ROW_HEAP, "FREE HEAP", heap);
  }

  if (now - s_lastUptimeTick >= 1000) {
    s_lastUptimeTick = now;
    String up = uptimeString();
    if (up != s_lastUptime) {
      s_lastUptime = up;
      paintRow(ROW_UPTIME, "UPTIME", up);
    }
  }
}

bool infoScreenConsumeClose() {
  if (!s_wantClose) return false;
  s_wantClose = false;
  return true;
}
