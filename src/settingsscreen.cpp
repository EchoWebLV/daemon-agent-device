#include "settingsscreen.h"
#include "devcfg.h"
#include "price.h"
#include "statusicons.h"
#include "touch.h"

#include <WiFi.h>

static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 16;

// Palette (RGB565) — same Daemon blue as the creature screen.
static constexpr uint16_t C_BG         = 0x0000;
static constexpr uint16_t C_ACCENT     = 0x0AFF;
static constexpr uint16_t C_ACCENT_HI  = 0x07FF;
static constexpr uint16_t C_TEXT       = 0xFFFF;
static constexpr uint16_t C_DIM        = 0x7BEF;
static constexpr uint16_t C_DIVIDER    = 0x18E3;
static constexpr uint16_t C_BAR_BG     = 0x2124;
static constexpr uint16_t C_BAR_FILL   = 0x04BF;

// Row layout. Each row has a label in the top-left and a "value/control"
// area on the right. Heights are measured including the divider below.
struct Row {
  const char *label;
  int16_t     y;        // top of row
  int16_t     h;        // height including divider
  // "Control" bounds used for hit-testing:
  int16_t     ctrlX, ctrlW;     // the slider bar / tappable zone
};

// Six rows: Wi-Fi is tallest (has IP+RSSI sub-line), sliders are medium,
// the three toggle rows (Bluetooth / Memory / Heartbeat) are compact.
// WI-FI now starts at y=32, below the X close button's hit region, so the
// two never overlap. Everything else shifts down to match.
static Row  ROWS[6] = {
  { "WI-FI",      32,  44, 120,  110 },
  { "BLUETOOTH",  76,  38, 170,   60 },
  { "VOLUME",     114, 42, 110,  120 },
  { "BRIGHTNESS", 156, 42, 110,  120 },
  { "MEMORY",     198, 38, 170,   60 },
  { "HEARTBEAT",  236, 38, 170,   60 },
};
static constexpr int R_WIFI = 0;
static constexpr int R_BT   = 1;
static constexpr int R_VOL  = 2;
static constexpr int R_BRI  = 3;
static constexpr int R_MEM  = 4;
static constexpr int R_HB   = 5;

static TFT_eSPI *s_tft = nullptr;

// Remember what we last drew so we only repaint changed rows.
static uint8_t  s_lastVol       = 255;
static uint8_t  s_lastBri       = 255;
static bool     s_lastBt        = false;
static bool     s_lastMem       = false;
static bool     s_lastHb        = false;
static String   s_lastSSID      = "\x01";
static String   s_lastPrice     = "\x01";

// Set to true when user taps the Wi-Fi row; main consumes it once.
static bool     s_wantWifiPanel = false;
// Set when user taps the X close button.
static bool     s_wantClose     = false;

// Close "X" button — top-right of status bar on every sub-screen.
// Hit region is deliberately bigger than the drawn pill so fat-finger
// taps near the corner reliably dismiss instead of wandering into the
// row below.
static constexpr int16_t CLOSE_W = 48;          // drawn pill stays 36-wide
static constexpr int16_t CLOSE_H = 30;          // extend below the visual
static constexpr int16_t CLOSE_X = SCR_W - CLOSE_W;
static constexpr int16_t CLOSE_Y = 0;
// Visual pill — narrower than the hit region. The wider hit region is
// only used for tap detection.
static constexpr int16_t CLOSE_DRAW_W = 36;
static constexpr int16_t CLOSE_DRAW_H = 24;
static constexpr int16_t CLOSE_DRAW_X = SCR_W - CLOSE_DRAW_W;

// --- Press / release tap detection ----------------------------------------
// We DO NOT fire row actions on finger-down, because a swipe-up gesture
// also begins with a finger-down and would incorrectly trigger the row
// under the starting point. Instead we remember where the finger landed
// and only fire the tap action if the finger lifts again within a small
// movement budget. Swipes simply exceed the threshold and are ignored by
// the tap handler (main.cpp still processes them as swipes).
static bool     s_pressed     = false;
static int16_t  s_pressX      = 0;
static int16_t  s_pressY      = 0;
static bool     s_pressMoved  = false;
static constexpr int16_t TAP_MOVE_BUDGET = 14;   // px

bool settingsScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

// ---------------------------------------------------------------------------
// Primitive painters
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  // Clear just the visible status-bar row (matches the drawn pill, not
  // the larger tap region).
  s_tft->fillRect(0, 0, SCR_W, CLOSE_DRAW_H, C_BG);
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(4, 4);
  s_tft->print("SETTINGS");

  // Price ticker just to the left of the close button
  String price = priceDisplayString();
  if (price.length() > 0) {
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_ACCENT_HI, C_BG);
    s_tft->drawString(price, CLOSE_DRAW_X - 4, 4);
  }
  s_lastPrice = price;

  // Feature indicators (memory + heartbeat) sit just left of the price so
  // they stay visible from every screen. Centring on the title's right half
  // keeps the SETTINGS label uncluttered.
  statusIconsDraw(s_tft, STATUS_H + 80, STATUS_H / 2, C_ACCENT_HI, C_BG);

  // Close button visual — drawn at its natural size; tap region below
  // is larger so fat-finger taps still catch it.
  s_tft->drawRoundRect(CLOSE_DRAW_X + 2, CLOSE_Y + 2,
                       CLOSE_DRAW_W - 4, CLOSE_DRAW_H - 4, 3, C_ACCENT);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->drawString("x",
                    CLOSE_DRAW_X + CLOSE_DRAW_W / 2,
                    CLOSE_Y + CLOSE_DRAW_H / 2);
}

// Draws the common label + divider for a row.
static void paintRowFrame(const Row &r) {
  s_tft->fillRect(0, r.y, SCR_W, r.h, C_BG);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(10, r.y + 8);
  s_tft->print(r.label);
  // divider
  s_tft->drawFastHLine(0, r.y + r.h - 1, SCR_W, C_DIVIDER);
}

static void paintWifi() {
  paintRowFrame(ROWS[R_WIFI]);
  String ssid = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("(offline)");
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TR_DATUM);
  s_tft->setTextColor(C_TEXT, C_BG);
  s_tft->drawString(ssid, SCR_W - 10, ROWS[R_WIFI].y + 8);
  // secondary line: IP or RSSI
  String sub;
  if (WiFi.status() == WL_CONNECTED) {
    sub = WiFi.localIP().toString();
    sub += "   ";
    sub += String(WiFi.RSSI());
    sub += " dBm";
  } else {
    sub = "not connected";
  }
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString(sub, SCR_W - 10, ROWS[R_WIFI].y + 28);
  s_lastSSID = ssid;
}

// Paint a compact on/off toggle row. Single-line layout: label on the left
// (drawn by paintRowFrame), a pill switch on the right. No ON/OFF text
// beneath — the pill fill colour conveys state clearly enough and keeps
// the row short so we can fit six rows on one screen.
static void paintToggleRow(const Row &r, bool on) {
  paintRowFrame(r);
  int16_t h = 20;
  int16_t x = r.ctrlX;
  int16_t y = r.y + (r.h - h) / 2 - 2;    // vertically centred in row
  int16_t w = r.ctrlW;
  s_tft->fillRoundRect(x, y, w, h, h / 2, on ? C_ACCENT : C_BAR_BG);
  int16_t kx = on ? (x + w - h) : x;
  s_tft->fillCircle(kx + h / 2, y + h / 2, h / 2 - 2, C_TEXT);
}

static void paintBluetooth() {
  paintToggleRow(ROWS[R_BT], devcfgBluetooth());
  s_lastBt = devcfgBluetooth();
}

static void paintMemory() {
  // "MEMORY" on the device now controls the Arweave archive — Irys is the
  // unified storage backend. The legacy Solana-memo path is still in the
  // codebase (toggleable via POST /config) but hidden from normal UI.
  paintToggleRow(ROWS[R_MEM], devcfgArweaveEnabled());
  s_lastMem = devcfgArweaveEnabled();
}

static void paintHeartbeat() {
  paintToggleRow(ROWS[R_HB], devcfgHeartbeatEnabled());
  s_lastHb = devcfgHeartbeatEnabled();
}

static void paintSlider(const Row &r, int value, int maxValue) {
  paintRowFrame(r);
  int16_t x = r.ctrlX;
  int16_t y = r.y + 20;
  int16_t w = r.ctrlW;
  int16_t h = 10;
  // track
  s_tft->fillRoundRect(x, y, w, h, h / 2, C_BAR_BG);
  // fill
  int fillW = (int)((long)value * w / maxValue);
  if (fillW > 0) s_tft->fillRoundRect(x, y, fillW, h, h / 2, C_BAR_FILL);
  // numeric value
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TR_DATUM);
  s_tft->setTextColor(C_TEXT, C_BG);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  s_tft->drawString(buf, SCR_W - 10, r.y + 8);
}

static void paintFooter() {
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString("swipe up to close", SCR_W / 2, SCR_H - 14);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void settingsScreenDraw() {
  if (!s_tft) return;

  // Reset tap-tracking + one-shot flags every time the screen is freshly
  // drawn so stale state from the previous visit (e.g. an in-progress
  // press that got interrupted by a screen transition) can't trigger a
  // ghost tap on the wifi row or any other control. This was the cause
  // of users getting bounced between Settings and Wi-Fi scanning.
  s_pressed        = false;
  s_pressMoved     = false;
  s_pressX         = 0;
  s_pressY         = 0;
  s_wantWifiPanel  = false;
  s_wantClose      = false;

  s_tft->fillScreen(C_BG);
  statusIconsResetCache();
  paintStatusBar();
  paintWifi();
  paintBluetooth();
  paintSlider(ROWS[R_VOL], devcfgVolume(),     21);
  paintSlider(ROWS[R_BRI], devcfgBrightness(), 255);
  paintMemory();
  paintHeartbeat();
  paintFooter();
  s_lastVol = devcfgVolume();
  s_lastBri = devcfgBrightness();
  Serial.println("settings: shown");
}

static bool inRect(int16_t x, int16_t y,
                   int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void handleInput() {
  int16_t x, y;
  bool    pressed = touchActive(x, y);

  // --- Sliders: act on continuous touch. Sliders need drag behaviour, so
  //     they don't participate in the press-release tap discrimination. ---
  if (pressed) {
    const Row &vr = ROWS[R_VOL];
    if (inRect(x, y, vr.ctrlX - 10, vr.y, vr.ctrlW + 20, vr.h)) {
      int rel = x - vr.ctrlX;
      if (rel < 0) rel = 0; if (rel > vr.ctrlW) rel = vr.ctrlW;
      int val = (int)((long)rel * 21 / vr.ctrlW);
      devcfgSetVolume((uint8_t)val);
    }
    const Row &br = ROWS[R_BRI];
    if (inRect(x, y, br.ctrlX - 10, br.y, br.ctrlW + 20, br.h)) {
      int rel = x - br.ctrlX;
      if (rel < 0) rel = 0; if (rel > br.ctrlW) rel = br.ctrlW;
      int val = 10 + (int)((long)rel * (255 - 10) / br.ctrlW);
      devcfgSetBrightness((uint8_t)val);
    }
  }

  // --- Tap tracking for non-slider rows (close, Wi-Fi, Bluetooth) ---
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
    // Finger just lifted — this is a candidate tap.
    s_pressed = false;
    if (s_pressMoved) return;          // swipe, not a tap — ignore

    int16_t tx = s_pressX, ty = s_pressY;
    if (inRect(tx, ty, CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H)) {
      s_wantClose = true;
      return;
    }
    const Row &wifi = ROWS[R_WIFI];
    if (inRect(tx, ty, 0, wifi.y, SCR_W, wifi.h)) {
      s_wantWifiPanel = true;
      return;
    }
    const Row &bt = ROWS[R_BT];
    if (inRect(tx, ty, 0, bt.y, SCR_W, bt.h)) {
      devcfgSetBluetooth(!devcfgBluetooth());
      return;
    }
    const Row &mem = ROWS[R_MEM];
    if (inRect(tx, ty, 0, mem.y, SCR_W, mem.h)) {
      devcfgSetArweaveEnabled(!devcfgArweaveEnabled());
      return;
    }
    const Row &hb = ROWS[R_HB];
    if (inRect(tx, ty, 0, hb.y, SCR_W, hb.h)) {
      devcfgSetHeartbeatEnabled(!devcfgHeartbeatEnabled());
      return;
    }
  }
}

bool settingsScreenConsumeWifiTap() {
  if (!s_wantWifiPanel) return false;
  s_wantWifiPanel = false;
  return true;
}

bool settingsScreenConsumeClose() {
  if (!s_wantClose) return false;
  s_wantClose = false;
  return true;
}

void settingsScreenTick() {
  if (!s_tft) return;
  handleInput();

  // Incremental repaint — only rows whose state changed.
  String price = priceDisplayString();
  if (price != s_lastPrice || statusIconsNeedRedraw()) {
    paintStatusBar();
  }

  String ssid = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("(offline)");
  if (ssid != s_lastSSID) paintWifi();

  if (devcfgBluetooth()     != s_lastBt)  paintBluetooth();
  if (devcfgArweaveEnabled() != s_lastMem) paintMemory();
  if (devcfgHeartbeatEnabled() != s_lastHb) paintHeartbeat();

  if (devcfgVolume() != s_lastVol) {
    paintSlider(ROWS[R_VOL], devcfgVolume(), 21);
    s_lastVol = devcfgVolume();
  }
  if (devcfgBrightness() != s_lastBri) {
    paintSlider(ROWS[R_BRI], devcfgBrightness(), 255);
    s_lastBri = devcfgBrightness();
  }
}
