#include "infoscreen.h"
#include "screenfx.h"
#include "wallet.h"

#include <WiFi.h>

static constexpr int16_t SCR_W = SCREENFX_W;
static constexpr int16_t SCR_H = SCREENFX_H;

// All colors now come from the shared UI_C_* palette in screenfx.h so the
// info page feels like the same OS as wallet / menu.
static constexpr uint16_t C_BG        = UI_C_BG;
static constexpr uint16_t C_PANEL     = UI_C_CARD;
static constexpr uint16_t C_PANEL_HI  = UI_C_CARD_HI;
static constexpr uint16_t C_ACCENT    = UI_C_WARN;       // info = amber
static constexpr uint16_t C_ACCENT_HI = UI_C_WARN_DIM;
static constexpr uint16_t C_TEXT      = UI_C_TEXT;
static constexpr uint16_t C_DIM       = UI_C_TEXT_DIM;
static constexpr uint16_t C_DIMMER    = UI_C_TEXT_DIMMER;

// Top bar geometry (matches the other panels).
static constexpr int16_t TOPBAR_Y = 3;
static constexpr int16_t CLOSE_W  = SCREENFX_X_BTN_W;
static constexpr int16_t CLOSE_H  = SCREENFX_X_BTN_H;
static constexpr int16_t CLOSE_X  = SCR_W - CLOSE_W - 6;
static constexpr int16_t CLOSE_Y  = TOPBAR_Y;

// Body starts just below the top bar, with a 10-px padding on each side.
static constexpr int16_t BODY_X     = 10;
static constexpr int16_t BODY_Y     = 34;
static constexpr int16_t BODY_W     = SCR_W - 2 * BODY_X;
static constexpr int16_t ROW_H      = 16;   // font-2 row height

static TFT_eSPI *s_tft = nullptr;

static bool     s_wantClose   = false;
static uint32_t s_lastTickMs  = 0;

// ---------------------------------------------------------------------------
// Row helpers
// ---------------------------------------------------------------------------
static void paintSectionHeader(int16_t &y, const char *label) {
  // Extra spacing above section headers so they breathe.
  y += 4;
  s_tft->fillRect(BODY_X, y, BODY_W, 1, C_PANEL_HI);
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(BODY_X, y + 3);
  s_tft->print(label);
  y += ROW_H - 2;
}

// Key: value row. Key left, value right-aligned.
static void paintKV(int16_t &y, const char *key, const String &value) {
  s_tft->setTextFont(2);

  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->setCursor(BODY_X, y);
  s_tft->print(key);

  s_tft->setTextDatum(TR_DATUM);
  s_tft->setTextColor(C_TEXT, C_BG);
  s_tft->drawString(value, BODY_X + BODY_W, y);

  y += ROW_H;
}

// ---------------------------------------------------------------------------
// Formatters
// ---------------------------------------------------------------------------
static String fmtMB(uint64_t bytes) {
  char buf[24];
  double mb = (double)bytes / (1024.0 * 1024.0);
  if (mb >= 10.0) snprintf(buf, sizeof(buf), "%.0f MB", mb);
  else            snprintf(buf, sizeof(buf), "%.1f MB", mb);
  return String(buf);
}

static String fmtKB(uint64_t bytes) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%u KB", (unsigned)(bytes / 1024));
  return String(buf);
}

static String fmtUptime(uint32_t ms) {
  uint32_t s = ms / 1000;
  uint32_t h = s / 3600;  s %= 3600;
  uint32_t m = s / 60;    s %= 60;
  char buf[32];
  if (h > 0)      snprintf(buf, sizeof(buf), "%uh %02um %02us", (unsigned)h, (unsigned)m, (unsigned)s);
  else if (m > 0) snprintf(buf, sizeof(buf), "%um %02us",       (unsigned)m, (unsigned)s);
  else            snprintf(buf, sizeof(buf), "%us",             (unsigned)s);
  return String(buf);
}

static String truncAddr(const String &p) {
  if (p.length() < 10) return p;
  return p.substring(0, 6) + "…" + p.substring(p.length() - 4);
}

// ---------------------------------------------------------------------------
// Painters
// ---------------------------------------------------------------------------
static void paintStatusBar() {
  s_tft->fillRect(0, 0, SCR_W, 30, C_BG);

  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(10, TOPBAR_Y + 4);
  s_tft->print("INFO");

  screenfxDrawXButton(s_tft, CLOSE_X, CLOSE_Y, C_ACCENT, C_BG);
}

// Everything below the status bar. Called on enter and on every tick.
static void paintBody() {
  // Clear the body area only (status bar stays put).
  s_tft->fillRect(0, 30, SCR_W, SCR_H - 30, C_BG);

  int16_t y = BODY_Y;

  // --- CHIP --------------------------------------------------------
  paintSectionHeader(y, "CHIP");
  {
    char model[32];
    snprintf(model, sizeof(model), "%s r%d", ESP.getChipModel(), ESP.getChipRevision());
    paintKV(y, "model",  model);
    char cpu[16];
    snprintf(cpu, sizeof(cpu), "%u MHz · %u cores",
             (unsigned)ESP.getCpuFreqMHz(), (unsigned)ESP.getChipCores());
    paintKV(y, "cpu",    cpu);
    paintKV(y, "flash",  fmtMB(ESP.getFlashChipSize()));
    paintKV(y, "psram",  fmtMB(ESP.getPsramSize()));
  }

  // --- MEMORY ------------------------------------------------------
  paintSectionHeader(y, "MEMORY");
  {
    paintKV(y, "heap free",  fmtKB(ESP.getFreeHeap()));
    paintKV(y, "psram free", fmtKB(ESP.getFreePsram()));
  }

  // --- FIRMWARE ----------------------------------------------------
  paintSectionHeader(y, "FIRMWARE");
  {
    paintKV(y, "built",  String(__DATE__) + " " + String(__TIME__));
    paintKV(y, "sdk",    String(ESP.getSdkVersion()));
    paintKV(y, "uptime", fmtUptime(millis()));
  }

  // --- NETWORK -----------------------------------------------------
  paintSectionHeader(y, "NETWORK");
  {
    if (WiFi.status() == WL_CONNECTED) {
      paintKV(y, "ssid", WiFi.SSID());
      paintKV(y, "ip",   WiFi.localIP().toString());
      char rssi[16];
      snprintf(rssi, sizeof(rssi), "%d dBm", WiFi.RSSI());
      paintKV(y, "rssi", rssi);
    } else {
      paintKV(y, "ssid", "(offline)");
    }
  }

  // --- WALLET ------------------------------------------------------
  paintSectionHeader(y, "WALLET");
  {
    paintKV(y, "pubkey",   truncAddr(walletPubkey()));
    paintKV(y, "can sign", walletCanSign() ? String("yes") : String("no"));
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool infoScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

static void fullPaint() {
  s_tft->fillScreen(C_BG);
  paintStatusBar();
  paintBody();
}

static void paintIntoSprite(TFT_eSPI *sprite) {
  TFT_eSPI *real = s_tft;
  s_tft = sprite;
  fullPaint();
  s_tft = real;
}

void infoScreenOnEnter() {
  s_wantClose = false;
  if (!screenfxSlideIn(s_tft, +SCR_H, paintIntoSprite)) {
    fullPaint();
  }
}

void infoScreenDraw() {
  if (!s_tft) return;
  fullPaint();
}

void infoScreenTick() {
  if (!s_tft) return;
  uint32_t now = millis();
  if (now - s_lastTickMs < 1000) return;   // 1 Hz is plenty for diagnostics
  s_lastTickMs = now;
  // Uptime / heap / RSSI all change — repaint the body only.
  paintBody();
}

void infoScreenHandleTap(int16_t x, int16_t y) {
  if (!s_tft) return;
  if (x >= CLOSE_X && x < CLOSE_X + CLOSE_W &&
      y >= CLOSE_Y && y < CLOSE_Y + CLOSE_H) {
    s_wantClose = true;
  }
}

bool infoScreenConsumeClose() {
  if (!s_wantClose) return false;
  s_wantClose = false;
  return true;
}
