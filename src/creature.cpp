#include "creature.h"
#include <math.h>
#include <vector>

// ---------------------------------------------------------------------------
// Screen geometry
// ---------------------------------------------------------------------------
static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 16;

// ---------------------------------------------------------------------------
// Palette (RGB565). Accent is #0F5DFD → 0x0AFF.
// The halo ramps are scaled-down versions of the same hue so the whole
// glow feels like a single colour at different intensities.
// ---------------------------------------------------------------------------
static constexpr uint16_t C_BG       = 0x0000;
static constexpr uint16_t C_ACCENT   = 0x0AFF;   // #0F5DFD — the brand colour
static constexpr uint16_t C_HALO_0   = 0x0043;   // ~10% — outermost glow
static constexpr uint16_t C_HALO_1   = 0x0086;   // ~20%
static constexpr uint16_t C_HALO_2   = 0x012C;   // ~40%
static constexpr uint16_t C_HALO_3   = 0x09D3;   // ~60%
static constexpr uint16_t C_MID      = 0x9EFF;   // pale cyan between rim+core
static constexpr uint16_t C_CORE     = 0xFFFF;   // pure white core

// Angry-mode variants (optional per-mood tint)
static constexpr uint16_t C_CORE_ANG = 0xFFE0;
static constexpr uint16_t C_GLOW_ANG = 0xF800;

static constexpr uint16_t C_STATUS   = 0x0AFF;

// ---------------------------------------------------------------------------
// Face geometry — the whole screen is black; the face rect covers the area
// where eyes + mouth animate. No body is rendered any more.
// ---------------------------------------------------------------------------
static constexpr int16_t FACE_X = 0;
static constexpr int16_t FACE_Y = 60;
static constexpr int16_t FACE_W = 240;
static constexpr int16_t FACE_H = 200;

// Eye centres (screen coordinates). 96 px apart so the inner halos don't
// kiss each other even at the widest part of the breathing animation.
static constexpr int16_t LEFT_EYE_CX  = 72;
static constexpr int16_t LEFT_EYE_CY  = 145;
static constexpr int16_t RIGHT_EYE_CX = 168;
static constexpr int16_t RIGHT_EYE_CY = 145;
static constexpr float   LEFT_EYE_ANGLE  =  0.34f;   // angry-inward slope
static constexpr float   RIGHT_EYE_ANGLE = -0.34f;

static constexpr int16_t MOUTH_CX = 120;
static constexpr int16_t MOUTH_CY = 215;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static TFT_eSPI    *s_tft       = nullptr;
static TFT_eSprite *s_faceBuf   = nullptr;   // per-frame double buffer
static CreatureMood s_mood      = MOOD_IDLE;
static bool         s_talking   = false;
static uint32_t     s_lastTickMs = 0;
static uint32_t     s_lastBlinkMs = 0;
static uint32_t     s_blinkStartMs = 0;
static bool         s_forceBlink = false;
static float        s_animPhase  = 0.0f;
static float        s_mouthPhase = 0.0f;
static float        s_mouthEnv   = 0.0f;

// Idle-animation extras: gentle bob, slow side-to-side gaze drift, and
// occasional "character moments" (double-blink, wink, micro-squint).
static float        s_bobPhase   = 0.0f;
static float        s_gazeX = 0.0f, s_gazeY = 0.0f;
static float        s_gazeTargetX = 0.0f, s_gazeTargetY = 0.0f;
static uint32_t     s_nextGazeMoveMs = 0;
static uint32_t     s_nextQuirkMs    = 0;
enum QuirkKind : uint8_t { QUIRK_NONE, QUIRK_DOUBLE_BLINK, QUIRK_WINK_L, QUIRK_WINK_R };
static QuirkKind    s_quirk         = QUIRK_NONE;
static uint32_t     s_quirkStartMs  = 0;
static int          s_quirkStage    = 0;
static String       s_status;
static String       s_lastStatusDrawn = "\x01invalid\x01";
static String       s_price;
static String       s_lastPriceDrawn  = "\x01invalid\x01";

// Subtitle state. The full string is word-wrapped into `s_subLines` when
// creatureSetSubtitle() is called; the subtitle panel shows 3 of those
// lines at a time and auto-advances while Daemon is talking so the user
// doesn't get stuck on just the first paragraph of a long reply.
static String               s_subText;
static std::vector<String>  s_subLines;
static uint32_t             s_subSetMs       = 0;
static int                  s_subScroll      = 0;  // index of first visible line
static int                  s_subScrollDrawn = -1; // last-drawn scroll pos
static String               s_lastSubKey     = "\x01invalid\x01";

// Subtitle panel layout (bottom of the screen).
static constexpr int16_t SUB_X = 0;
static constexpr int16_t SUB_Y = 272;
static constexpr int16_t SUB_W = 240;
static constexpr int16_t SUB_H = 48;
static constexpr uint16_t C_SUB_TEXT = 0xFFFF;

// ---------------------------------------------------------------------------
// Eye pill — tapered almond. Multiple concentric layers produce the glow.
// ---------------------------------------------------------------------------
static void drawEyePill(TFT_eSprite *s, int cx, int cy, float angle,
                        float halfLen, float thickness, uint16_t color) {
  int steps = max(18, (int)(halfLen * 2.0f));
  for (int i = 0; i <= steps; ++i) {
    float t   = (float)i / (float)steps;
    float off = (t - 0.5f) * 2.0f * halfLen;
    float tap = sinf(t * (float)PI);        // 0..1..0 taper
    int   r   = (int)(thickness * tap);
    if (r <= 0) continue;
    int x = cx + (int)(cosf(angle) * off);
    int y = cy + (int)(sinf(angle) * off);
    s->fillCircle(x, y, r, color);
  }
}

static void drawEye(TFT_eSprite *s, int cxScreen, int cyScreen, float angle,
                    float pulse, float blinkT, CreatureMood mood) {
  int cx = cxScreen - FACE_X;
  int cy = cyScreen - FACE_Y;

  // Almond size. Smaller than the first bare-face pass so the two eyes
  // don't overlap even with the glow halo fully rendered.
  float baseHalfLen   = 34.0f;
  float baseThickness = 16.0f;

  if (mood == MOOD_LISTEN) baseThickness += 2.5f;
  if (mood == MOOD_HAPPY)  baseThickness *= 0.55f;   // squinty smile
  if (mood == MOOD_THINK)  baseThickness *= 0.9f;

  float scale = 1.0f + 0.10f * pulse;          // gentle swell

  if (blinkT >= 0.0f) {
    float c = 0.5f * (1.0f - cosf(blinkT * 2.0f * (float)PI));
    float closure = 1.0f - c;
    scale *= 0.15f + 0.85f * closure;
  }

  float halfLen   = baseHalfLen * (1.0f + 0.03f * pulse);
  float thickness = baseThickness * scale;

  uint16_t cHalo0 = C_HALO_0;
  uint16_t cHalo1 = C_HALO_1;
  uint16_t cHalo2 = C_HALO_2;
  uint16_t cHalo3 = C_HALO_3;
  uint16_t cGlow  = (mood == MOOD_ANGRY) ? C_GLOW_ANG : C_ACCENT;
  uint16_t cMid   = C_MID;
  uint16_t cCore  = (mood == MOOD_ANGRY) ? C_CORE_ANG : C_CORE;

  // Seven-layer glow: far → near.
  drawEyePill(s, cx, cy, angle, halfLen + 18.0f, thickness + 19.0f, cHalo0);
  drawEyePill(s, cx, cy, angle, halfLen + 12.0f, thickness + 13.0f, cHalo1);
  drawEyePill(s, cx, cy, angle, halfLen + 7.0f,  thickness + 8.0f,  cHalo2);
  drawEyePill(s, cx, cy, angle, halfLen + 3.0f,  thickness + 4.0f,  cHalo3);
  drawEyePill(s, cx, cy, angle, halfLen,         thickness,         cGlow);
  drawEyePill(s, cx, cy, angle, halfLen * 0.82f, thickness * 0.72f, cMid);
  drawEyePill(s, cx, cy, angle, halfLen * 0.55f, thickness * 0.5f,  cCore);
}

// ---------------------------------------------------------------------------
// Mouth — an unfilled #0F5DFD capsule, scales vertically when talking.
// ---------------------------------------------------------------------------
static void drawMouth(TFT_eSprite *s, float openness, CreatureMood mood,
                      int yOffset = 0) {
  int cx = MOUTH_CX - FACE_X;
  int cy = MOUTH_CY - FACE_Y + yOffset;

  if (openness < 0.0f) openness = 0.0f;
  if (openness > 1.0f) openness = 1.0f;

  if (mood == MOOD_HAPPY) {
    // Big smile arc
    for (int dx = -24; dx <= 24; dx += 1) {
      int yy = cy + (int)(10.0f - (dx * dx) / 80.0f);
      s->fillCircle(cx + dx, yy, 3, C_ACCENT);
    }
    return;
  }

  int halfW = 24;
  int halfH = 4 + (int)(openness * 16.0f);

  // Outer glow haloes (subtle)
  for (int layer = 2; layer >= 0; --layer) {
    uint16_t c = (layer == 2) ? C_HALO_1 :
                 (layer == 1) ? C_HALO_2 : C_ACCENT;
    int extra = (layer + 1) * 2;
    s->fillCircle(cx - halfW, cy, halfH + extra, c);
    s->fillCircle(cx + halfW, cy, halfH + extra, c);
    s->fillRect(cx - halfW, cy - (halfH + extra),
                halfW * 2, (halfH + extra) * 2, c);
  }

  // Hollow interior
  s->fillCircle(cx - halfW, cy, halfH, C_BG);
  s->fillCircle(cx + halfW, cy, halfH, C_BG);
  s->fillRect(cx - halfW, cy - halfH, halfW * 2, halfH * 2, C_BG);

  // Tiny tongue blob at the bottom, grows with openness.
  int tongueY = cy + halfH - 2;
  int tongueR = 5 + (int)(openness * 3.0f);
  s->fillCircle(cx, tongueY + tongueR / 2, tongueR, C_ACCENT);
}

// ---------------------------------------------------------------------------
// Status bar (left: status text, right: price ticker)
// ---------------------------------------------------------------------------
static void drawStatusIfChanged(bool force) {
  bool statusChanged = s_status != s_lastStatusDrawn;
  bool priceChanged  = s_price  != s_lastPriceDrawn;
  if (!force && !statusChanged && !priceChanged) return;

  s_tft->fillRect(0, 0, SCR_W, STATUS_H, C_BG);
  s_tft->setTextFont(1);

  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_STATUS, C_BG);
  s_tft->setCursor(4, 4);
  s_tft->print(s_status);

  if (s_price.length() > 0) {
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_ACCENT, C_BG);
    s_tft->drawString(s_price, SCR_W - 4, 4);
  }

  s_lastStatusDrawn = s_status;
  s_lastPriceDrawn  = s_price;
}

// ---------------------------------------------------------------------------
// Subtitle — bottom-of-screen word-wrap panel. Full text is word-wrapped
// ahead of time into s_subLines; the panel shows a sliding window of
// MAX_SUB_LINES lines and advances while Daemon is talking so long
// replies get fully read out on screen rather than stuck at the top.
// ---------------------------------------------------------------------------
static constexpr int SUB_LINE_H  = 16;
static constexpr int MAX_SUB_LINES = SUB_H / SUB_LINE_H;     // 3

// Wrap text into lines that fit within the subtitle panel width. Text is
// split greedily at spaces, with hard breaks for single oversized words.
static void rewrapSubtitle() {
  s_subLines.clear();
  if (!s_tft || s_subText.length() == 0) return;

  s_tft->setTextFont(2);
  const int maxW = SUB_W - 8;

  String remaining = s_subText;
  while (remaining.length() > 0) {
    String current = "";
    while (remaining.length() > 0) {
      int sp = remaining.indexOf(' ');
      String word = (sp < 0) ? remaining : remaining.substring(0, sp);
      String tryLine = current.length() ? current + " " + word : word;
      if (s_tft->textWidth(tryLine) > maxW) {
        if (current.length() == 0) {
          // Word wider than the panel — chop it.
          while (word.length() > 0 &&
                 s_tft->textWidth(word) > maxW) {
            word.remove(word.length() - 1);
          }
          current  = word;
          remaining = (sp < 0) ? "" : remaining.substring(sp + 1);
        }
        break;
      }
      current  = tryLine;
      remaining = (sp < 0) ? "" : remaining.substring(sp + 1);
    }
    if (current.length() == 0) break;   // safety guard
    s_subLines.push_back(current);
  }
}

static void drawSubtitleIfChanged(bool force) {
  // Cheap key so we only repaint when either the text or the visible
  // window actually changed.
  String key = s_subText + "\x01" + String(s_subScroll);
  if (!force && key == s_lastSubKey) return;
  s_lastSubKey = key;

  s_tft->fillRect(SUB_X, SUB_Y, SUB_W, SUB_H, C_BG);
  if (s_subLines.empty()) return;

  s_tft->setTextFont(2);
  s_tft->setTextDatum(TC_DATUM);
  s_tft->setTextColor(C_SUB_TEXT, C_BG);
  const int cx = SUB_X + SUB_W / 2;

  int start = s_subScroll;
  int end   = min((int)s_subLines.size(), start + MAX_SUB_LINES);
  int y = SUB_Y + 2;
  for (int i = start; i < end; ++i) {
    s_tft->drawString(s_subLines[i], cx, y);
    y += SUB_LINE_H;
  }
  s_subScrollDrawn = s_subScroll;
}

// Advance the scroll window while Daemon is talking. Scrolling starts
// shortly after the subtitle is set (so the user can read the first page)
// and then moves one line at a time at a reading pace that roughly tracks
// the speech rate. Stops once we've reached the final page.
static void tickSubtitleScroll(uint32_t now) {
  if (s_subLines.size() <= MAX_SUB_LINES) {
    s_subScroll = 0;
    return;
  }
  const int  maxScroll   = (int)s_subLines.size() - MAX_SUB_LINES;
  const uint32_t LEAD_IN = 1400;   // ms before we start scrolling
  const uint32_t PER_LINE =
      (s_talking || s_mood == MOOD_TALK) ? 1800 : 2400;  // faster while speaking

  uint32_t elapsed = now - s_subSetMs;
  if (elapsed < LEAD_IN) { s_subScroll = 0; return; }
  int target = (int)((elapsed - LEAD_IN) / PER_LINE);
  if (target > maxScroll) target = maxScroll;
  s_subScroll = target;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool creatureBegin(TFT_eSPI *tft) {
  s_tft = tft;
  tft->fillScreen(C_BG);

  s_faceBuf = new TFT_eSprite(tft);
  s_faceBuf->setColorDepth(16);
  // Deliberately DO NOT enable PSRAM — TFT_eSPI on ESP32-S3 pushes PSRAM
  // sprites through a byte-wise path that mangles RGB565 endianness (our
  // neon blue 0x0AFF flips to 0xFF0A which is yellow-orange). 96 KB fits
  // comfortably in internal DRAM and keeps the color correct.
  if (!s_faceBuf->createSprite(FACE_W, FACE_H)) {
    Serial.println("creature: faceBuf sprite alloc failed");
    return false;
  }

  s_lastTickMs  = millis();
  s_lastBlinkMs = millis();
  drawStatusIfChanged(true);
  drawSubtitleIfChanged(true);
  return true;
}

void creatureRepaint() {
  if (!s_tft) return;
  s_tft->fillScreen(C_BG);
  s_lastStatusDrawn = "\x01invalid\x01";
  s_lastPriceDrawn  = "\x01invalid\x01";
  s_lastSubKey      = "\x01invalid\x01";
  drawStatusIfChanged(true);
  drawSubtitleIfChanged(true);
}

void creatureSetMood(CreatureMood m)    { s_mood = m; }
void creatureSetTalking(bool on)        { s_talking = on; if (!on) s_mouthEnv = 0.0f; }
void creatureForceBlink()               { s_forceBlink = true; }
void creatureSetStatus(const String &s) { s_status = s; }
void creatureSetPrice (const String &s) { s_price  = s; }
void creatureSetSubtitle(const String &text) {
  if (text == s_subText) return;        // no change → keep scroll state
  s_subText   = text;
  s_subScroll = 0;
  s_subSetMs  = millis();
  rewrapSubtitle();
}

void creatureTick() {
  if (!s_faceBuf) return;

  uint32_t now = millis();
  float dt = (now - s_lastTickMs) / 1000.0f;
  if (dt > 0.25f) dt = 0.25f;
  s_lastTickMs = now;

  // Breathing pulse (eye brightness + slight scale).
  s_animPhase += dt * (s_mood == MOOD_LISTEN ? 2.4f : 1.5f);
  if (s_animPhase > 2.0f * (float)PI) s_animPhase -= 2.0f * (float)PI;
  float pulse = sinf(s_animPhase);

  // Vertical bob — slow sine at 0.35 Hz, ±2 px. Makes the face feel alive.
  s_bobPhase += dt * 2.2f;
  int16_t bobDY = (int16_t)(sinf(s_bobPhase) * 2.0f);

  // Gaze drift — pick a random target every 5-10 s, lerp toward it.
  if (now > s_nextGazeMoveMs) {
    s_gazeTargetX = (float)random(-6, 7);   // ±6 px
    s_gazeTargetY = (float)random(-3, 4);   // ±3 px
    s_nextGazeMoveMs = now + (uint32_t)random(5000, 11000);
  }
  s_gazeX += (s_gazeTargetX - s_gazeX) * min(1.0f, dt * 1.5f);
  s_gazeY += (s_gazeTargetY - s_gazeY) * min(1.0f, dt * 1.5f);

  // Quirk scheduling — every 12-22 s fire a small character moment.
  if (s_quirk == QUIRK_NONE && now > s_nextQuirkMs) {
    int roll = random(0, 3);
    s_quirk = (roll == 0) ? QUIRK_DOUBLE_BLINK
            : (roll == 1) ? QUIRK_WINK_L
                          : QUIRK_WINK_R;
    s_quirkStartMs = now;
    s_quirkStage = 0;
    s_nextQuirkMs = now + (uint32_t)random(12000, 22000);
  }

  // Per-eye blink phase. Quirks override the normal blink timer.
  float blinkL = -1.0f, blinkR = -1.0f;

  auto standardBlink = [&]() -> float {
    if (s_forceBlink && now - s_blinkStartMs > 400) {
      s_blinkStartMs = now;
      s_forceBlink = false;
    }
    if (now - s_blinkStartMs < 200) {
      return (now - s_blinkStartMs) / 200.0f;
    }
    if (now - s_lastBlinkMs > (uint32_t)random(3000, 6000)) {
      s_blinkStartMs = now;
      s_lastBlinkMs  = now;
    }
    return -1.0f;
  };

  if (s_quirk == QUIRK_DOUBLE_BLINK) {
    // Two blinks 250 ms apart
    uint32_t elapsed = now - s_quirkStartMs;
    if (elapsed < 200)      { blinkL = blinkR = elapsed / 200.0f; }
    else if (elapsed < 450) { /* gap */ }
    else if (elapsed < 650) { blinkL = blinkR = (elapsed - 450) / 200.0f; }
    else                    { s_quirk = QUIRK_NONE; }
  } else if (s_quirk == QUIRK_WINK_L || s_quirk == QUIRK_WINK_R) {
    uint32_t elapsed = now - s_quirkStartMs;
    if (elapsed < 400) {
      float t = elapsed / 400.0f;                // slower wink
      if (s_quirk == QUIRK_WINK_L) blinkL = t;
      else                         blinkR = t;
    } else { s_quirk = QUIRK_NONE; }
  } else {
    float b = standardBlink();
    blinkL = b;
    blinkR = b;
  }

  // Mouth drive — slight breathing motion even when idle.
  float targetOpen = 0.0f;
  if (s_talking || s_mood == MOOD_TALK) {
    s_mouthPhase += dt * 13.0f;
    float a = 0.5f * (1.0f + sinf(s_mouthPhase));
    float b = 0.5f * (1.0f + sinf(s_mouthPhase * 0.37f + 1.1f));
    targetOpen = a * 0.7f + b * 0.3f;
    if (((int)(s_mouthPhase * 0.3f)) % 7 == 0) targetOpen *= 0.3f;
  } else {
    // tiny idle twitch
    targetOpen = 0.05f + 0.03f * sinf(s_animPhase * 0.5f);
  }
  s_mouthEnv += (targetOpen - s_mouthEnv) * min(1.0f, dt * 18.0f);

  s_faceBuf->fillSprite(C_BG);
  int16_t leftX  = LEFT_EYE_CX  + (int16_t)s_gazeX;
  int16_t rightX = RIGHT_EYE_CX + (int16_t)s_gazeX;
  int16_t eyeY   = LEFT_EYE_CY  + (int16_t)s_gazeY + bobDY;
  drawEye(s_faceBuf, leftX,  eyeY, LEFT_EYE_ANGLE,  pulse, blinkL, s_mood);
  drawEye(s_faceBuf, rightX, eyeY, RIGHT_EYE_ANGLE, pulse, blinkR, s_mood);
  drawMouth(s_faceBuf, s_mouthEnv, s_mood, bobDY);
  s_faceBuf->pushSprite(FACE_X, FACE_Y);

  drawStatusIfChanged(false);
  tickSubtitleScroll(now);
  drawSubtitleIfChanged(false);
}
