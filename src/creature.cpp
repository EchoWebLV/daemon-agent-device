#include "creature.h"
#include <math.h>

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
static String       s_status;
static String       s_lastStatusDrawn = "\x01invalid\x01";
static String       s_price;
static String       s_lastPriceDrawn  = "\x01invalid\x01";
static String       s_subText;
static String       s_lastSubDrawn    = "\x01invalid\x01";

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
static void drawMouth(TFT_eSprite *s, float openness, CreatureMood mood) {
  int cx = MOUTH_CX - FACE_X;
  int cy = MOUTH_CY - FACE_Y;

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
// Subtitle — bottom-of-screen word-wrap panel. No prefix; every line is
// center-aligned so longer replies look deliberately formatted instead of
// ragged-left.
// ---------------------------------------------------------------------------
static void drawSubtitleIfChanged(bool force) {
  if (!force && s_subText == s_lastSubDrawn) return;
  s_lastSubDrawn = s_subText;

  s_tft->fillRect(SUB_X, SUB_Y, SUB_W, SUB_H, C_BG);
  if (s_subText.length() == 0) return;

  s_tft->setTextFont(2);
  s_tft->setTextDatum(TC_DATUM);                 // top-centered
  s_tft->setTextColor(C_SUB_TEXT, C_BG);

  const int lineH    = 16;
  const int maxLines = SUB_H / lineH;             // 3
  const int maxW     = SUB_W - 8;                 // 4 px margin each side
  const int cx       = SUB_X + SUB_W / 2;

  String remaining = s_subText;
  int y = SUB_Y + 2;
  int line = 0;
  while (remaining.length() > 0 && line < maxLines) {
    String current = "";
    while (remaining.length() > 0) {
      int sp = remaining.indexOf(' ');
      String word = (sp < 0) ? remaining : remaining.substring(0, sp);
      String tryLine = current.length() ? current + " " + word : word;
      if (s_tft->textWidth(tryLine) > maxW) {
        if (current.length() == 0) {
          while (word.length() > 0 &&
                 s_tft->textWidth(word) > maxW) {
            word.remove(word.length() - 1);
          }
          current = word;
          remaining = (sp < 0) ? "" : remaining.substring(sp + 1);
        }
        break;
      }
      current = tryLine;
      remaining = (sp < 0) ? "" : remaining.substring(sp + 1);
    }
    if (line == maxLines - 1 && remaining.length() > 0) {
      while (current.length() > 0 &&
             s_tft->textWidth(current + "...") > maxW) {
        current.remove(current.length() - 1);
      }
      current += "...";
    }
    s_tft->drawString(current, cx, y);
    y += lineH;
    line++;
  }
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
  s_lastSubDrawn    = "\x01invalid\x01";
  drawStatusIfChanged(true);
  drawSubtitleIfChanged(true);
}

void creatureSetMood(CreatureMood m)    { s_mood = m; }
void creatureSetTalking(bool on)        { s_talking = on; if (!on) s_mouthEnv = 0.0f; }
void creatureForceBlink()               { s_forceBlink = true; }
void creatureSetStatus(const String &s) { s_status = s; }
void creatureSetPrice (const String &s) { s_price  = s; }
void creatureSetSubtitle(const String &text) {
  s_subText = text;
}

void creatureTick() {
  if (!s_faceBuf) return;

  uint32_t now = millis();
  float dt = (now - s_lastTickMs) / 1000.0f;
  if (dt > 0.25f) dt = 0.25f;
  s_lastTickMs = now;

  s_animPhase += dt * (s_mood == MOOD_LISTEN ? 2.4f : 1.5f);
  if (s_animPhase > 2.0f * (float)PI) s_animPhase -= 2.0f * (float)PI;
  float pulse = sinf(s_animPhase);

  float blinkT = -1.0f;
  if (s_forceBlink && now - s_blinkStartMs > 400) {
    s_blinkStartMs = now;
    s_forceBlink = false;
  }
  bool blinking = (now - s_blinkStartMs) < 200;
  if (blinking) {
    blinkT = (now - s_blinkStartMs) / 200.0f;
  } else if (now - s_lastBlinkMs > (uint32_t)random(3000, 6000)) {
    s_blinkStartMs = now;
    s_lastBlinkMs = now;
  }

  float targetOpen = 0.0f;
  if (s_talking || s_mood == MOOD_TALK) {
    s_mouthPhase += dt * 13.0f;
    float a = 0.5f * (1.0f + sinf(s_mouthPhase));
    float b = 0.5f * (1.0f + sinf(s_mouthPhase * 0.37f + 1.1f));
    targetOpen = a * 0.7f + b * 0.3f;
    if (((int)(s_mouthPhase * 0.3f)) % 7 == 0) targetOpen *= 0.3f;
  }
  s_mouthEnv += (targetOpen - s_mouthEnv) * min(1.0f, dt * 18.0f);

  // Black background, then eyes + mouth directly into the face buffer.
  s_faceBuf->fillSprite(C_BG);
  drawEye(s_faceBuf, LEFT_EYE_CX,  LEFT_EYE_CY,  LEFT_EYE_ANGLE,
          pulse, blinkT, s_mood);
  drawEye(s_faceBuf, RIGHT_EYE_CX, RIGHT_EYE_CY, RIGHT_EYE_ANGLE,
          pulse, blinkT, s_mood);
  drawMouth(s_faceBuf, s_mouthEnv, s_mood);
  s_faceBuf->pushSprite(FACE_X, FACE_Y);

  drawStatusIfChanged(false);
  drawSubtitleIfChanged(false);
}
