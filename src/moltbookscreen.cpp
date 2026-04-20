#include "moltbookscreen.h"
#include "devcfg.h"
#include "price.h"
#include "statusicons.h"
#include "touch.h"
#include "moltbook.h"

// ---------------------------------------------------------------------------
// Geometry / palette — same dark Daemon look as xpostscreen.
// ---------------------------------------------------------------------------
static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;

static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_CARD      = 0x0861;
static constexpr uint16_t C_ACCENT    = 0xFB20;   // orange-ish for Moltbook (lobster!)
static constexpr uint16_t C_ACCENT_HI = 0xFD20;
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_DIM       = 0x7BEF;
static constexpr uint16_t C_RED       = 0xF800;
static constexpr uint16_t C_GREEN     = 0x07E0;
static constexpr uint16_t C_DIVIDER   = 0x18E3;

static constexpr int16_t CLOSE_W = 36;
static constexpr int16_t CLOSE_H = 24;
static constexpr int16_t CLOSE_X = SCR_W - CLOSE_W;
static constexpr int16_t CLOSE_Y = 0;

static constexpr int16_t STATUS_BAND_Y = CLOSE_H;
static constexpr int16_t STATUS_BAND_H = 34;

static constexpr int16_t LIST_TOP      = STATUS_BAND_Y + STATUS_BAND_H + 2;
static constexpr int16_t FOOTER_Y      = SCR_H - 14;
static constexpr int16_t LIST_BOT      = FOOTER_Y - 4;

static constexpr int16_t CARD_X      = 8;
static constexpr int16_t CARD_W      = SCR_W - 16;
static constexpr int16_t CARD_H      = 54;
static constexpr int16_t CARD_GAP    = 4;

static TFT_eSPI *s_tft         = nullptr;
static bool     s_wantClose    = false;

static uint32_t s_lastTickMs       = 0;
static uint32_t s_lastDrawnCount   = (uint32_t)-1;
static String   s_lastDrawnErr     = "\x01";
static bool     s_lastDrawnEnabled = false;
static uint32_t s_lastDrawnLastMs  = 0;

static bool     s_pressed     = false;
static int16_t  s_pressX      = 0;
static int16_t  s_pressY      = 0;
static bool     s_pressMoved  = false;
static constexpr int16_t TAP_MOVE_BUDGET = 14;

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

static void wrapText(const String &text, int maxPx,
                     String *out, int cap, int *countOut) {
  int n = text.length();
  int line = 0;
  int i = 0;
  char scratch[288];

  while (i < n && line < cap) {
    while (i < n && text[i] == ' ') ++i;
    if (i >= n) break;

    int lineStart = i;
    int lastSpace = -1;
    int scratchLen = 0;
    int fitted     = i;

    while (fitted < n && scratchLen < (int)sizeof(scratch) - 1) {
      scratch[scratchLen] = text[fitted];
      scratch[scratchLen + 1] = '\0';
      int w = s_tft->textWidth(scratch);
      if (w > maxPx) break;
      scratchLen++;
      if (text[fitted] == ' ') lastSpace = fitted;
      fitted++;
    }

    int end = fitted;
    if (end <= lineStart) end = lineStart + 1;
    if (end < n && lastSpace > lineStart) end = lastSpace;
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
  s_tft->print("MOLTBOOK");

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

  const bool enabled = devcfgMoltbookEnabled();
  const bool hasCred = devcfgMoltbookApiKey().length() > 0;

  s_tft->setTextFont(2);
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setCursor(8, STATUS_BAND_Y + 3);
  if (!hasCred) {
    s_tft->setTextColor(C_RED, C_BG);
    s_tft->print("NEEDS API KEY");
  } else if (!enabled) {
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->print("OFF");
  } else {
    s_tft->setTextColor(C_GREEN, C_BG);
    s_tft->print("ON");
    s_tft->setTextColor(C_DIM, C_BG);
    char buf[32];
    snprintf(buf, sizeof(buf), "  every %u min", (unsigned)devcfgMoltbookIntervalMin());
    s_tft->print(buf);
  }

  s_tft->setCursor(8, STATUS_BAND_Y + 18);
  String err = moltbookLastError();
  if (err.length() > 0) {
    s_tft->setTextColor(C_RED, C_BG);
    String label = "err: " + err;
    if (label.length() > 40) label = label.substring(0, 39) + "\xe2\x80\xa6";
    s_tft->print(label);
  } else {
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->print("last: ");
    s_tft->print(ago(moltbookLastSuccessMs()));
  }

  s_lastDrawnEnabled = enabled;
  s_lastDrawnErr     = err;
  s_lastDrawnLastMs  = moltbookLastSuccessMs();
}

static void paintList() {
  s_tft->fillRect(0, LIST_TOP, SCR_W, LIST_BOT - LIST_TOP, C_BG);

  MoltbookRecent recents[6];
  size_t n = moltbookGetRecent(recents, sizeof(recents) / sizeof(recents[0]));
  s_lastDrawnCount = (uint32_t)n;

  if (n == 0) {
    s_tft->setTextFont(2);
    s_tft->setTextDatum(TC_DATUM);
    s_tft->setTextColor(C_DIM, C_BG);
    s_tft->drawString("(no posts yet)", SCR_W / 2, LIST_TOP + 24);
    if (devcfgMoltbookApiKey().length() == 0) {
      s_tft->setTextFont(1);
      s_tft->drawString("set API key in web portal",
                        SCR_W / 2, LIST_TOP + 52);
    } else if (!devcfgMoltbookEnabled()) {
      s_tft->setTextFont(1);
      s_tft->drawString("enable Moltbook in Settings",
                        SCR_W / 2, LIST_TOP + 52);
    }
    return;
  }

  int y = LIST_TOP + 2;
  for (size_t i = 0; i < n; ++i) {
    if (y + CARD_H > LIST_BOT) break;
    const MoltbookRecent &p = recents[i];

    s_tft->fillRoundRect(CARD_X, y, CARD_W, CARD_H, 6, C_CARD);

    s_tft->setTextFont(1);
    String lines[2];
    int lineCount = 0;
    wrapText(p.text, CARD_W - 18, lines, 2, &lineCount);
    s_tft->setTextDatum(TL_DATUM);
    s_tft->setTextColor(C_TEXT, C_CARD);
    for (int li = 0; li < lineCount; ++li) {
      s_tft->setCursor(CARD_X + 8, y + 4 + li * 12);
      s_tft->print(lines[li]);
    }
    if (lineCount == 2 &&
        ((int)p.text.length() >
         (int)(lines[0].length() + lines[1].length() + 2))) {
      s_tft->setCursor(CARD_X + CARD_W - 16, y + 4 + 1 * 12);
      s_tft->print("\xe2\x80\xa6");
    }

    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_ACCENT, C_CARD);
    s_tft->drawString(ago(p.postedMs), CARD_X + CARD_W - 6, y + CARD_H - 12);

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
bool moltbookScreenBegin(TFT_eSPI *tft) {
  s_tft = tft;
  return tft != nullptr;
}

void moltbookScreenDraw() {
  if (!s_tft) return;
  s_tft->fillScreen(C_BG);
  statusIconsResetCache();
  paintTitle();
  paintStatusBand();
  paintList();
  paintFooter();
  s_lastTickMs = millis();
}

void moltbookScreenDrawTo(TFT_eSprite *target) {
  if (!target) return;
  TFT_eSPI *saved = s_tft;
  s_tft = target;
  moltbookScreenDraw();
  s_tft = saved;
  statusIconsResetCache();
}

void moltbookScreenTick() {
  if (!s_tft) return;
  handleInput();

  uint32_t now = millis();
  if (now - s_lastTickMs < 2000) return;
  s_lastTickMs = now;

  if (statusIconsNeedRedraw()) paintTitle();

  bool     en   = devcfgMoltbookEnabled();
  String   err  = moltbookLastError();
  uint32_t lm   = moltbookLastSuccessMs();
  size_t   cnt  = moltbookRecentCount();

  bool stateChanged = (en != s_lastDrawnEnabled) ||
                      (err != s_lastDrawnErr)    ||
                      (lm  != s_lastDrawnLastMs);

  static uint32_t s_agoLastPaint = 0;
  bool agoStale = (now - s_agoLastPaint) > 10000;

  if (stateChanged || agoStale) {
    paintStatusBand();
    s_agoLastPaint = now;
  }

  if ((uint32_t)cnt != s_lastDrawnCount) {
    paintList();
    s_lastDrawnCount = (uint32_t)cnt;
  }
}

bool moltbookScreenConsumeClose() {
  if (!s_wantClose) return false;
  s_wantClose = false;
  return true;
}
