#include "wifiscreen.h"
#include "server.h"
#include "touch.h"
#include "devcfg.h"
#include "screenfx.h"

#include <WiFi.h>
#include <vector>

// ---------------------------------------------------------------------------
// Geometry / palette
// ---------------------------------------------------------------------------
static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 16;

static constexpr uint16_t C_BG       = 0x0000;
static constexpr uint16_t C_ACCENT   = 0x0AFF;
static constexpr uint16_t C_ACCENT2  = 0x04BF;
static constexpr uint16_t C_TEXT     = 0xFFFF;
static constexpr uint16_t C_DIM      = 0x7BEF;
static constexpr uint16_t C_GREEN    = 0x07E0;
static constexpr uint16_t C_RED      = 0xF800;
static constexpr uint16_t C_DIV      = 0x18E3;
static constexpr uint16_t C_KEY_BG   = 0x2965;
static constexpr uint16_t C_KEY_HI   = 0x04BF;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
enum Mode : uint8_t {
  MODE_SCANNING   = 0,   // blocking scan in progress
  MODE_LIST       = 1,   // showing network list
  MODE_KEYBOARD   = 2,   // typing password
  MODE_CONNECTING = 3,   // attempting connection
  MODE_RESULT     = 4,   // showing success / failure message
};

static TFT_eSPI *s_tft          = nullptr;
static Mode      s_mode         = MODE_SCANNING;
static bool      s_wantExit     = false;
// Set exactly by the top-right X button tap. Distinct from s_wantExit so
// main.cpp can route "user asked to go home" to the creature screen,
// while automatic exits (swipe, successful connect) still fall back to
// Settings.
static bool      s_wantHome     = false;
static bool      s_needFullDraw = false;

struct Net {
  String  ssid;
  int32_t rssi;
  uint8_t enc;
};
static std::vector<Net> s_nets;
static int   s_listScroll    = 0;    // first visible index
static String s_selectedSSID;
static uint8_t s_selectedEnc = 0;

// Keyboard state
static String s_password;
static bool   s_shift = false;
static uint32_t s_resultShownAt = 0;
static String s_resultMsg;
static bool   s_resultOk = false;

// Scan state (defined + populated further down). Declared here so earlier
// helpers like goBackOneStep() can reference them.
static bool     s_scanInFlight;
static uint32_t s_scanStartMs;

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
static bool inRect(int16_t x, int16_t y,
                   int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// Close "X" button — same 28×24 rect as every other panel, via
// screenfxDrawXButton. The tap hit-rect matches the visual exactly.
static constexpr int16_t CLOSE_W = SCREENFX_X_BTN_W;   // 28
static constexpr int16_t CLOSE_H = SCREENFX_X_BTN_H;   // 24
static constexpr int16_t CLOSE_X = SCR_W - CLOSE_W - 6;
static constexpr int16_t CLOSE_Y = 2;

// Press / release tap tracking — same pattern as settingsscreen, to keep
// "start of a swipe" from being interpreted as a button tap.
static bool     s_pressed     = false;
static int16_t  s_pressX      = 0;
static int16_t  s_pressY      = 0;
static bool     s_pressMoved  = false;
static constexpr int16_t TAP_MOVE_BUDGET = 14;

// Returns true on the frame the finger lifts within the move budget, i.e.
// this was a tap rather than a swipe. Writes the press-down position.
static bool consumeTap(int16_t &tx, int16_t &ty) {
  int16_t x, y;
  bool pressed = touchActive(x, y);
  if (pressed) {
    if (!s_pressed) {
      s_pressed = true; s_pressX = x; s_pressY = y; s_pressMoved = false;
    } else if (abs(x - s_pressX) > TAP_MOVE_BUDGET ||
               abs(y - s_pressY) > TAP_MOVE_BUDGET) {
      s_pressMoved = true;
    }
    return false;
  }
  if (!s_pressed) return false;
  s_pressed = false;
  if (s_pressMoved) return false;
  tx = s_pressX; ty = s_pressY;
  return true;
}

static void drawStatusBar(const char *title) {
  s_tft->fillRect(0, 0, SCR_W, CLOSE_H + CLOSE_Y + 2, C_BG);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(10, CLOSE_Y + 4);
  s_tft->print(title);

  screenfxDrawXButton(s_tft, CLOSE_X, CLOSE_Y, C_ACCENT, C_BG);
}

// True if the tap hit the X button at the top-right.
static bool closeButtonTapped(int16_t x, int16_t y) {
  return inRect(x, y, CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H);
}

// Mode-aware "back": keyboard/connecting/result drops back to the list;
// scanning + list drop out of the Wi-Fi panel entirely.
static void goBackOneStep() {
  // Cancel any in-flight async scan so it doesn't leave stale results.
  if (s_scanInFlight) {
    WiFi.scanDelete();
    s_scanInFlight = false;
  }
  switch (s_mode) {
    case MODE_KEYBOARD:
    case MODE_CONNECTING:
    case MODE_RESULT:
      s_mode = MODE_LIST;
      s_needFullDraw = true;
      break;
    case MODE_SCANNING:
    case MODE_LIST:
    default:
      s_wantExit = true;
      break;
  }
}

// ---------------------------------------------------------------------------
// Scan — async. A blocking WiFi.scanNetworks() freezes the UI for ~4 s and
// makes the X/swipe unresponsive during that window, which is the bug the
// user just hit. With async scan we keep ticking touch on every frame.
// (Storage declared above near the other state variables.)
// ---------------------------------------------------------------------------
static void kickScan() {
  // WiFi.scanNetworks(async=true) returns immediately; we poll scanComplete.
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  s_scanInFlight = true;
  s_scanStartMs  = millis();
}

static void ingestScanResults(int n) {
  s_nets.clear();
  for (int i = 0; i < n; ++i) {
    Net net;
    net.ssid = WiFi.SSID(i);
    net.rssi = WiFi.RSSI(i);
    net.enc  = WiFi.encryptionType(i);
    if (net.ssid.length() > 0) s_nets.push_back(net);
  }
  WiFi.scanDelete();
  s_listScroll = 0;
}

// ---------------------------------------------------------------------------
// Network LIST drawing
// ---------------------------------------------------------------------------
static constexpr int16_t LIST_ROW_H      = 34;
static constexpr int16_t LIST_TOP_Y      = 80;
static constexpr int16_t LIST_ROWS_VISIBLE = 5;
// Buttons
static constexpr int16_t BTN_W = 105, BTN_H = 28;
static constexpr int16_t BTN_DISC_X = 10,  BTN_DISC_Y = 44;
static constexpr int16_t BTN_SCAN_X = 125, BTN_SCAN_Y = 44;
static constexpr int16_t BTN_UP_X   = 190, BTN_UP_Y   = LIST_TOP_Y - 22;
static constexpr int16_t BTN_DOWN_X = 190, BTN_DOWN_Y = LIST_TOP_Y + LIST_ROW_H * LIST_ROWS_VISIBLE - 20;

static void drawRssiBars(int16_t x, int16_t y, int32_t rssi) {
  int bars = 0;
  if (rssi > -55) bars = 4;
  else if (rssi > -65) bars = 3;
  else if (rssi > -75) bars = 2;
  else if (rssi > -85) bars = 1;
  for (int i = 0; i < 4; ++i) {
    uint16_t c = (i < bars) ? C_ACCENT : C_DIV;
    s_tft->fillRect(x + i * 4, y + (6 - i * 2), 3, 2 + i * 2, c);
  }
}

static void drawButton(int16_t x, int16_t y, int16_t w, int16_t h,
                       const char *label, uint16_t bg, uint16_t fg) {
  s_tft->fillRoundRect(x, y, w, h, 5, bg);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(fg, bg);
  s_tft->drawString(label, x + w / 2, y + h / 2);
}

static void drawList() {
  s_tft->fillScreen(C_BG);
  drawStatusBar("WI-FI");

  // Current connection line
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->setCursor(10, 22);
  s_tft->print("connected: ");
  s_tft->setTextColor(C_TEXT, C_BG);
  s_tft->print(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("(none)"));

  // Action buttons
  drawButton(BTN_DISC_X, BTN_DISC_Y, BTN_W, BTN_H, "disconnect",
             WiFi.status() == WL_CONNECTED ? C_RED : C_KEY_BG,
             C_TEXT);
  drawButton(BTN_SCAN_X, BTN_SCAN_Y, BTN_W, BTN_H, "rescan",
             C_ACCENT2, C_TEXT);

  // List rows
  s_tft->drawFastHLine(0, LIST_TOP_Y - 4, SCR_W, C_DIV);
  int end = s_listScroll + LIST_ROWS_VISIBLE;
  if (end > (int)s_nets.size()) end = s_nets.size();
  for (int i = s_listScroll; i < end; ++i) {
    int slot = i - s_listScroll;
    int16_t y = LIST_TOP_Y + slot * LIST_ROW_H;
    const Net &n = s_nets[i];
    // row bg
    s_tft->fillRect(0, y, SCR_W, LIST_ROW_H - 1, C_BG);
    // ssid
    s_tft->setTextColor(C_TEXT, C_BG);
    s_tft->setCursor(10, y + 8);
    String s = n.ssid;
    if (s.length() > 22) { s.remove(22); s += "…"; }
    s_tft->print(s);
    // lock icon for secured networks
    if (n.enc != WIFI_AUTH_OPEN) {
      s_tft->setTextColor(C_DIM, C_BG);
      s_tft->drawString("*", 170, y + 8);
    }
    // signal bars
    drawRssiBars(SCR_W - 25, y + 10, n.rssi);
    // divider
    s_tft->drawFastHLine(0, y + LIST_ROW_H - 1, SCR_W, C_DIV);
  }

  // Scroll up/down buttons
  if (s_listScroll > 0) {
    drawButton(BTN_UP_X - 15, BTN_UP_Y, 40, 20, "up", C_KEY_BG, C_TEXT);
  }
  if (end < (int)s_nets.size()) {
    drawButton(BTN_DOWN_X - 15, BTN_DOWN_Y, 40, 20, "dn", C_KEY_BG, C_TEXT);
  }

  // Footer
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString("swipe right to go back", SCR_W / 2, SCR_H - 12);
}

static void handleListInput() {
  int16_t x, y;
  if (!consumeTap(x, y)) return;

  if (closeButtonTapped(x, y)) { s_wantHome = true; return; }

  if (inRect(x, y, BTN_DISC_X, BTN_DISC_Y, BTN_W, BTN_H)) {
    serverWifiDisconnect();
    drawList();
    return;
  }
  if (inRect(x, y, BTN_SCAN_X, BTN_SCAN_Y, BTN_W, BTN_H)) {
    s_mode = MODE_SCANNING;
    s_needFullDraw = true;
    return;
  }
  // scroll buttons
  if (s_listScroll > 0 &&
      inRect(x, y, BTN_UP_X - 15, BTN_UP_Y, 40, 20)) {
    s_listScroll = max(0, s_listScroll - LIST_ROWS_VISIBLE);
    drawList();
    return;
  }
  int end = s_listScroll + LIST_ROWS_VISIBLE;
  if (end < (int)s_nets.size() &&
      inRect(x, y, BTN_DOWN_X - 15, BTN_DOWN_Y, 40, 20)) {
    s_listScroll = min((int)s_nets.size() - LIST_ROWS_VISIBLE,
                       s_listScroll + LIST_ROWS_VISIBLE);
    drawList();
    return;
  }
  // tap on a network row?
  for (int i = s_listScroll;
       i < s_listScroll + LIST_ROWS_VISIBLE && i < (int)s_nets.size(); ++i) {
    int slot = i - s_listScroll;
    int16_t ry = LIST_TOP_Y + slot * LIST_ROW_H;
    if (inRect(x, y, 0, ry, SCR_W, LIST_ROW_H - 1)) {
      const Net &n = s_nets[i];
      s_selectedSSID = n.ssid;
      s_selectedEnc  = n.enc;
      if (n.enc == WIFI_AUTH_OPEN) {
        // open network — connect immediately
        s_password = "";
        s_mode = MODE_CONNECTING;
        s_needFullDraw = true;
      } else {
        s_password = "";
        s_shift = false;
        s_mode = MODE_KEYBOARD;
        s_needFullDraw = true;
      }
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// On-screen keyboard (QWERTY + numeric row)
// ---------------------------------------------------------------------------
// Layout:
//   y=20      title
//   y=46      password field
//   y=90      [cancel] [clear] [done]
//   y=130     1234567890
//   y=160     qwertyuiop
//   y=190     asdfghjkl   (9 keys centered)
//   y=220     shift zxcvbnm back
//   y=250     mode  space  enter
// key cell: 24x28
static constexpr int16_t KB_Y0 = 130;
static constexpr int16_t KB_KW = 24;
static constexpr int16_t KB_KH = 28;

static const char *ROW0 = "1234567890";
static const char *ROW1 = "qwertyuiop";
static const char *ROW2 = "asdfghjkl";
static const char *ROW3 = "zxcvbnm";

static void drawKey(int16_t x, int16_t y, int16_t w, int16_t h,
                    const char *label, uint16_t bg) {
  s_tft->fillRoundRect(x + 1, y + 1, w - 2, h - 2, 4, bg);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(C_TEXT, bg);
  s_tft->drawString(label, x + w / 2, y + h / 2);
}

static void drawPasswordField() {
  s_tft->fillRect(10, 46, SCR_W - 20, 28, C_KEY_BG);
  s_tft->drawRoundRect(10, 46, SCR_W - 20, 28, 4, C_ACCENT2);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(ML_DATUM);
  s_tft->setTextColor(C_TEXT, C_KEY_BG);
  // Show last few plaintext chars so user can see what they just typed;
  // mask the rest with dots.
  String shown;
  int len = s_password.length();
  for (int i = 0; i < len - 2 && i < 32; ++i) shown += '*';
  if (len >= 2) shown += s_password.substring(len - 2);
  else          shown  = s_password;
  s_tft->drawString(shown, 16, 46 + 14);
}

static void drawKeyboard() {
  s_tft->fillScreen(C_BG);
  drawStatusBar("WI-FI PASSWORD");

  // Title (selected SSID)
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->drawString(s_selectedSSID, SCR_W / 2, 22);

  drawPasswordField();

  // Action buttons
  drawButton(8,   88, 70, 24, "cancel", C_RED,    C_TEXT);
  drawButton(86,  88, 70, 24, "clear",  C_KEY_BG, C_TEXT);
  drawButton(164, 88, 70, 24, "done",   C_GREEN,  0x0000);

  // Row 0: numbers (10 keys)
  for (int i = 0; i < 10; ++i) {
    char c = ROW0[i];
    char buf[2] = { c, 0 };
    drawKey(i * KB_KW, KB_Y0, KB_KW, KB_KH, buf, C_KEY_BG);
  }
  // Row 1: qwertyuiop (10 keys)
  for (int i = 0; i < 10; ++i) {
    char c = ROW1[i];
    if (s_shift) c = toupper(c);
    char buf[2] = { c, 0 };
    drawKey(i * KB_KW, KB_Y0 + KB_KH, KB_KW, KB_KH, buf, C_KEY_BG);
  }
  // Row 2: asdfghjkl (9 keys, centered → 12px left margin)
  for (int i = 0; i < 9; ++i) {
    char c = ROW2[i];
    if (s_shift) c = toupper(c);
    char buf[2] = { c, 0 };
    drawKey(12 + i * KB_KW, KB_Y0 + KB_KH * 2, KB_KW, KB_KH, buf, C_KEY_BG);
  }
  // Row 3: shift(36) zxcvbnm(7x24) back(36)
  drawKey(0, KB_Y0 + KB_KH * 3, 36, KB_KH,
          s_shift ? "^^" : "^",
          s_shift ? C_KEY_HI : C_KEY_BG);
  for (int i = 0; i < 7; ++i) {
    char c = ROW3[i];
    if (s_shift) c = toupper(c);
    char buf[2] = { c, 0 };
    drawKey(36 + i * KB_KW, KB_Y0 + KB_KH * 3, KB_KW, KB_KH, buf, C_KEY_BG);
  }
  drawKey(204, KB_Y0 + KB_KH * 3, 36, KB_KH, "<", C_KEY_BG);

  // Row 4: special (symbols placeholder, space, enter)
  drawKey(0,  KB_Y0 + KB_KH * 4, 60, KB_KH,   ".-_",  C_KEY_BG);
  drawKey(60, KB_Y0 + KB_KH * 4, 120, KB_KH,  "space", C_KEY_BG);
  drawKey(180,KB_Y0 + KB_KH * 4, 60, KB_KH,   "enter", C_ACCENT2);
}

// Returns the character (or 0) for a tap at (x,y). Special negative codes
// for shift(-1), backspace(-2), space(-3), enter(-4), symbols(-5),
// cancel(-10), clear(-11), done(-12).
static int keyAt(int16_t x, int16_t y) {
  // Action buttons
  if (inRect(x, y, 8, 88, 70, 24))   return -10;
  if (inRect(x, y, 86, 88, 70, 24))  return -11;
  if (inRect(x, y, 164, 88, 70, 24)) return -12;

  if (y < KB_Y0) return 0;
  int row = (y - KB_Y0) / KB_KH;
  if (row < 0 || row > 4) return 0;
  int col = -1;

  if (row == 0) {
    col = x / KB_KW;
    if (col >= 0 && col < 10) return (int)ROW0[col];
  } else if (row == 1) {
    col = x / KB_KW;
    if (col >= 0 && col < 10) {
      char c = ROW1[col];
      return s_shift ? toupper(c) : c;
    }
  } else if (row == 2) {
    int lx = x - 12;
    col = lx / KB_KW;
    if (col >= 0 && col < 9) {
      char c = ROW2[col];
      return s_shift ? toupper(c) : c;
    }
  } else if (row == 3) {
    if (x < 36)         return -1;    // shift
    if (x >= 204)       return -2;    // backspace
    int lx = x - 36;
    col = lx / KB_KW;
    if (col >= 0 && col < 7) {
      char c = ROW3[col];
      return s_shift ? toupper(c) : c;
    }
  } else if (row == 4) {
    if (x < 60)   return -5;          // symbols
    if (x < 180)  return -3;          // space
    return -4;                        // enter / done
  }
  return 0;
}

static const char *SYMBOLS = ".-_@!?#$%&*/+=:";
static int s_symbolIdx = 0;

static void handleKeyboardInput() {
  int16_t x, y;
  if (!consumeTap(x, y)) return;

  // The X button (in the status bar) means "go home" — close the whole
  // Wi-Fi flow, not just this mode.
  if (closeButtonTapped(x, y)) {
    s_wantHome = true;
    return;
  }

  int k = keyAt(x, y);
  if (k == 0) return;

  if (k >= 32 && k < 127) {
    if (s_password.length() < 63) s_password += (char)k;
    drawPasswordField();
    if (s_shift) { s_shift = false; drawKeyboard(); }
  } else if (k == -1) {
    s_shift = !s_shift;
    drawKeyboard();
  } else if (k == -2) {
    if (s_password.length() > 0) s_password.remove(s_password.length() - 1);
    drawPasswordField();
  } else if (k == -3) {
    if (s_password.length() < 63) s_password += ' ';
    drawPasswordField();
  } else if (k == -4 || k == -12) {
    s_mode = MODE_CONNECTING;
    s_needFullDraw = true;
  } else if (k == -5) {
    // cycle through the symbol palette, inserting one symbol per tap
    if (s_password.length() < 63) {
      s_password += SYMBOLS[s_symbolIdx];
      s_symbolIdx = (s_symbolIdx + 1) % strlen(SYMBOLS);
    }
    drawPasswordField();
  } else if (k == -10) {
    s_mode = MODE_LIST;
    s_needFullDraw = true;
  } else if (k == -11) {
    s_password = "";
    drawPasswordField();
  }
}

// ---------------------------------------------------------------------------
// CONNECTING / RESULT
// ---------------------------------------------------------------------------
static void drawCenteredMessage(const char *title, const char *subtitle,
                                uint16_t titleColor) {
  s_tft->fillScreen(C_BG);
  drawStatusBar("WI-FI");   // already includes the close X
  s_tft->setTextFont(4);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(titleColor, C_BG);
  s_tft->drawString(title, SCR_W / 2, SCR_H / 2 - 20);
  if (subtitle && *subtitle) {
    s_tft->setTextFont(2);
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->drawString(subtitle, SCR_W / 2, SCR_H / 2 + 10);
  }
}

static void attemptConnection() {
  drawCenteredMessage("connecting…", s_selectedSSID.c_str(), C_ACCENT);
  bool ok = serverWifiConnect(s_selectedSSID, s_password);
  s_resultOk = ok;
  s_resultMsg = ok ? "connected" : "failed";
  s_resultShownAt = millis();
  s_mode = MODE_RESULT;
  drawCenteredMessage(s_resultMsg.c_str(),
                      ok ? WiFi.localIP().toString().c_str() : "check password",
                      ok ? C_GREEN : C_RED);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool wifiScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

void wifiScreenEnter() {
  s_mode = MODE_SCANNING;
  s_needFullDraw = true;
  s_wantExit = false;
  s_wantHome = false;
  s_password = "";
  s_selectedSSID = "";
  s_listScroll = 0;
}

bool wifiScreenConsumeExit() {
  if (!s_wantExit) return false;
  s_wantExit = false;
  return true;
}

bool wifiScreenConsumeCloseHome() {
  if (!s_wantHome) return false;
  s_wantHome = false;
  return true;
}

void wifiScreenHandleSwipe(SwipeDir sw) {
  // Any "back-ish" swipe inside the Wi-Fi flow drops to the list first,
  // only exits the panel when the user is already on the list.
  if (sw == SWIPE_RIGHT || sw == SWIPE_UP || sw == SWIPE_DOWN) {
    goBackOneStep();
  }
}

void wifiScreenTick() {
  if (!s_tft) return;

  switch (s_mode) {
    case MODE_SCANNING: {
      if (s_needFullDraw) {
        drawCenteredMessage("scanning…", nullptr, C_ACCENT);
        s_needFullDraw = false;
        if (!s_scanInFlight) kickScan();
      }

      // Allow cancelling while scan runs: a tap on the X button or any
      // swipe back exits the Wi-Fi panel straight away.
      int16_t tx, ty;
      if (consumeTap(tx, ty) && closeButtonTapped(tx, ty)) {
        WiFi.scanDelete();
        s_scanInFlight = false;
        s_wantHome = true;
        break;
      }

      int res = WiFi.scanComplete();
      if (res >= 0) {
        ingestScanResults(res);
        s_scanInFlight = false;
        s_mode = MODE_LIST;
        s_needFullDraw = true;
      } else if (res == WIFI_SCAN_FAILED) {
        Serial.println("wifi scan failed");
        s_scanInFlight = false;
        s_mode = MODE_LIST;
        s_needFullDraw = true;
      } else if (millis() - s_scanStartMs > 20000) {
        // Safety net — scanComplete() has been seen to get stuck at -1.
        Serial.println("wifi scan timed out");
        WiFi.scanDelete();
        s_scanInFlight = false;
        s_mode = MODE_LIST;
        s_needFullDraw = true;
      }
      // Otherwise keep rendering the "scanning…" screen; we just fall
      // through and let the rest of loop() run (animation, touch, etc).
      break;
    }

    case MODE_LIST:
      if (s_needFullDraw) { drawList(); s_needFullDraw = false; }
      handleListInput();
      break;

    case MODE_KEYBOARD:
      if (s_needFullDraw) { drawKeyboard(); s_needFullDraw = false; }
      handleKeyboardInput();
      break;

    case MODE_CONNECTING:
      if (s_needFullDraw) {
        s_needFullDraw = false;
        attemptConnection();     // transitions to MODE_RESULT
      }
      break;

    case MODE_RESULT:
      // Linger 2.5 s then go back to list (refreshed) or keyboard on fail.
      if (millis() - s_resultShownAt > 2500) {
        if (s_resultOk) {
          s_wantExit = true;       // success — pop back to Settings
        } else {
          s_mode = MODE_KEYBOARD;
          s_needFullDraw = true;
        }
      }
      break;
  }
}
