#include "xpostscreen.h"
#include "devcfg.h"
#include "price.h"
#include "statusicons.h"
#include "touch.h"
#include "xpost.h"

// ---------------------------------------------------------------------------
// Geometry / palette — same dark Daemon look as the other sub-screens.
// ---------------------------------------------------------------------------
static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;

static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_CARD      = 0x0861;
static constexpr uint16_t C_ACCENT    = 0x0AFF;
static constexpr uint16_t C_ACCENT_HI = 0x07FF;
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_DIM       = 0x7BEF;
static constexpr uint16_t C_RED       = 0xF800;
static constexpr uint16_t C_GREEN     = 0x07E0;
static constexpr uint16_t C_DIVIDER   = 0x18E3;

static constexpr int16_t CLOSE_W = 36;
static constexpr int16_t CLOSE_H = 24;
static constexpr int16_t CLOSE_X = SCR_W - CLOSE_W;
static constexpr int16_t CLOSE_Y = 0;

// Status band just under the title bar summarising poster state (on/off,
// interval, last error). 240×32 px.
static constexpr int16_t STATUS_BAND_Y = CLOSE_H;
static constexpr int16_t STATUS_BAND_H = 34;

// List area: below the status band, above the footer. Each tweet card is
// a rounded rect with the text wrapped across up to 3 lines and a
// relative-time sub-line.
static constexpr int16_t LIST_TOP      = STATUS_BAND_Y + STATUS_BAND_H + 2;   // ~60
static constexpr int16_t FOOTER_Y      = SCR_H - 14;
static constexpr int16_t LIST_BOT      = FOOTER_Y - 4;

static constexpr int16_t CARD_X      = 8;
static constexpr int16_t CARD_W      = SCR_W - 16;
static constexpr int16_t CARD_H      = 54;
static constexpr int16_t CARD_GAP    = 4;

static TFT_eSPI *s_tft         = nullptr;
static bool     s_wantClose    = false;

// Cached values so we only repaint when something changes.
static uint32_t s_lastTickMs       = 0;
static uint32_t s_lastDrawnCount   = (uint32_t)-1;   // of recent tweets
static String   s_lastDrawnErr     = "\x01";
static bool     s_lastDrawnEnabled = false;
static uint32_t s_lastDrawnLastMs  = 0;

// Press / release tap tracking — for the close "x" only.
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

static String ago(uint32_t sinceMs) {
  if (sinceMs == 0) return String("never");
  uint32_t dMs = millis() - sinceMs;
  uint32_t s = dMs / 1000;
  if (s < 60)       return String(s) + "s ago";
  uint32_t m = s / 60;
  if (m < 60)       return String(m) + "m ago";
  uint32_t h = m / 60;
  if (h < 24)       return String(h) + "h ago";
  return String(h / 24) + "d ago";
}

// Wrap a string into lines that fit within `maxPx` using the current font.
// Greedy word-boundary wrapping; falls back to mid-word cuts for super
// long runs (e.g. URLs with no spaces).
static void wrapText(const String &text, int maxPx,
                     String *out, int cap, int *countOut) {
  int line = 0;
  int i = 0;
  int n = text.length();
  while (i < n && line < cap) {
    // Skip leading spaces on a new line.
    while (i < n && text[i] == ' ') ++i;
    if (i >= n) break;
    int lineStart = i;
    int lastSpace = -1;
    int fitted = i;
    while (fitted <= n) {
      String candidate = text.substring(lineStart, fitted);
      int w = s_tft->textWidth(candidate.c_str());
      if (w > maxPx) break;
      fitted++;
      if (fitted - 1 < n && text[fitted - 1] == ' ') lastSpace = fitted - 1;
    }
    int end = fitted - 1;
    if (end <= lineStart) end = lineStart + 1;          // force progress
    if (end < n) {
      // Prefer the last space we saw so we don't mid-word break.
      if (lastSpace > lineStart) end = lastSpace;
    }
    out[line++] = text.substring(lineStart, end);
    i = end;
  }
  *countOut = line;
}

// ---------------------------------------------------------------------------
// Painters
// ---------------------------------------------------------------------------
static void paintTitle() {
  s_tft->fillRect(0, 0, SCR_W, CLOSE_H, C_BG);
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->setCursor(4, 4);
  s_tft->print("X POSTS");

  String price = priceDisplayString();
  if (price.length() > 0) {
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_ACCENT_HI, C_BG);
    s_tft->drawString(price, CLOSE_X - 4, 4);
  }

  statusIconsDraw(s_tft, CLOSE_H + 40, CLOSE_H / 2, C_ACCENT_HI, C_BG);

  s_tft->drawRoundRect(CLOSE_X + 2, CLOSE_Y + 2, CLOSE_W - 4, CLOSE_H - 4, 3, C_ACCENT);
  s_tft->setTextFont(2);
  s_tft->setTextDatum(MC_DATUM);
  s_tft->setTextColor(C_ACCENT, C_BG);
  s_tft->drawString("x", CLOSE_X + CLOSE_W / 2, CLOSE_Y + CLOSE_H / 2);
}

static void paintStatusBand() {
  s_tft->fillRect(0, STATUS_BAND_Y, SCR_W, STATUS_BAND_H, C_BG);
  s_tft->drawFastHLine(0, STATUS_BAND_Y + STATUS_BAND_H - 1, SCR_W, C_DIVIDER);

  const bool enabled = devcfgXPostEnabled();
  const bool creds   = devcfgXCredentialsPresent();

  // Line 1: "ON • every 60m" / "OFF" / "NEEDS KEYS"
  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setCursor(8, STATUS_BAND_Y + 3);
  if (!creds) {
    s_tft->setTextColor(C_RED, C_BG);
    s_tft->print("NEEDS API KEYS");
  } else if (!enabled) {
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->print("OFF");
  } else {
    s_tft->setTextColor(C_GREEN, C_BG);
    s_tft->print("ON");
    s_tft->setTextColor(C_DIM, C_BG);
    char buf[32];
    snprintf(buf, sizeof(buf), "  every %u min", (unsigned)devcfgXPostIntervalMin());
    s_tft->print(buf);
  }

  // Line 2: "last: 12m ago" / "err: ..."
  s_tft->setCursor(8, STATUS_BAND_Y + 18);
  String err = xpostLastError();
  if (err.length() > 0) {
    s_tft->setTextColor(C_RED, C_BG);
    String label = "err: " + err;
    if (label.length() > 40) label = label.substring(0, 39) + "…";
    s_tft->print(label);
  } else {
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->print("last: ");
    s_tft->print(ago(xpostLastSuccessMs()));
  }

  s_lastDrawnEnabled = enabled;
  s_lastDrawnErr     = err;
  s_lastDrawnLastMs  = xpostLastSuccessMs();
}

// Paint the list of recent tweets. Simple fixed-height cards; if there
// isn't one yet we show an "(no posts yet)" placeholder.
static void paintList() {
  s_tft->fillRect(0, LIST_TOP, SCR_W, LIST_BOT - LIST_TOP, C_BG);

  XPostRecent recents[6];
  size_t n = xpostGetRecent(recents, sizeof(recents) / sizeof(recents[0]));
  s_lastDrawnCount = (uint32_t)n;

  if (n == 0) {
    s_tft->setTextFont(2);
    s_tft->setTextDatum(TC_DATUM);
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->drawString("(no posts yet)", SCR_W / 2, LIST_TOP + 24);
    if (!devcfgXCredentialsPresent()) {
      s_tft->setTextFont(1);
      s_tft->drawString("open the web portal to set API keys",
                        SCR_W / 2, LIST_TOP + 52);
    } else if (!devcfgXPostEnabled()) {
      s_tft->setTextFont(1);
      s_tft->drawString("enable X POST in Settings",
                        SCR_W / 2, LIST_TOP + 52);
    }
    return;
  }

  int y = LIST_TOP + 2;
  for (size_t i = 0; i < n; ++i) {
    if (y + CARD_H > LIST_BOT) break;
    const XPostRecent &t = recents[i];

    s_tft->fillRoundRect(CARD_X, y, CARD_W, CARD_H, 6, C_CARD);

    // Up-to-two lines of tweet text.
    s_tft->setTextFont(1);
    String lines[2];
    int lineCount = 0;
    wrapText(t.text, CARD_W - 18, lines, 2, &lineCount);
    s_tft->setTextDatum(TL_DATUM);
    s_tft->setTextColor(C_TEXT, C_CARD);
    for (int li = 0; li < lineCount; ++li) {
      s_tft->setCursor(CARD_X + 8, y + 4 + li * 12);
      s_tft->print(lines[li]);
    }
    // If the text got truncated to 2 lines, append an ellipsis on the
    // second line so the user knows there's more.
    if (lineCount == 2 &&
        ((int)t.text.length() >
         (int)(lines[0].length() + lines[1].length() + 2))) {
      s_tft->setCursor(CARD_X + CARD_W - 16, y + 4 + 1 * 12);
      s_tft->print("…");
    }

    // Relative-time sub-line on the bottom right.
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_ACCENT, C_CARD);
    s_tft->drawString(ago(t.postedMs), CARD_X + CARD_W - 6, y + CARD_H - 12);

    y += CARD_H + CARD_GAP;
  }
}

static void paintFooter() {
  s_tft->fillRect(0, FOOTER_Y - 2, SCR_W, SCR_H - FOOTER_Y + 2, C_BG);
  s_tft->setTextFont(1);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(C_DIM, C_BG);
  s_tft->drawString("swipe down to close", SCR_W / 2, FOOTER_Y);
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
bool xpostScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

void xpostScreenDraw() {
  if (!s_tft) return;
  s_tft->fillScreen(C_BG);
  statusIconsResetCache();
  paintTitle();
  paintStatusBand();
  paintList();
  paintFooter();
  s_lastTickMs = millis();
}

void xpostScreenTick() {
  if (!s_tft) return;
  handleInput();

  // Throttle to ~2 Hz — the status band needs to tick the "last: 12m ago"
  // clock forward and the list grows asynchronously when posts succeed.
  uint32_t now = millis();
  if (now - s_lastTickMs < 500) return;
  s_lastTickMs = now;

  if (statusIconsNeedRedraw()) paintTitle();

  // Repaint status band when enabled flag, error, or last-post timestamp
  // changes — otherwise the "last: Xm ago" string would be stale.
  bool   en   = devcfgXPostEnabled();
  String err  = xpostLastError();
  uint32_t lm = xpostLastSuccessMs();
  if (en != s_lastDrawnEnabled || err != s_lastDrawnErr || lm != s_lastDrawnLastMs) {
    paintStatusBand();
  } else {
    // Still needs to retick the "Xm ago" clock even if nothing structural
    // changed. Cheap — the band is small.
    paintStatusBand();
  }

  // Repaint the list when a new tweet arrives (count changed) or on
  // every tick when there are entries, so the relative-time labels
  // ("12s ago") stay roughly fresh.
  size_t cnt = xpostRecentCount();
  if ((uint32_t)cnt != s_lastDrawnCount || cnt > 0) {
    paintList();
  }
}

bool xpostScreenConsumeClose() {
  if (!s_wantClose) return false;
  s_wantClose = false;
  return true;
}
