#include "creature.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Screen geometry
// ---------------------------------------------------------------------------
static constexpr int16_t SCR_W    = 240;
static constexpr int16_t SCR_H    = 320;
static constexpr int16_t STATUS_H = 16;

// Width of the neon body outline (pixels). The reference image uses a
// thick, uniform stroke so we avoid any glow halo around the body.
static constexpr int16_t OUTLINE_W = 4;

// ---------------------------------------------------------------------------
// Palette (RGB565)
// ---------------------------------------------------------------------------
static constexpr uint16_t C_BG           = 0x0000;  // pure black
static constexpr uint16_t C_BODY         = 0x0000;  // black interior
static constexpr uint16_t C_OUTLINE      = 0x049F;  // bright neon blue
static constexpr uint16_t C_EYE_HALO_0   = 0x0009;  // very dim blue (far glow)
static constexpr uint16_t C_EYE_HALO_1   = 0x0013;  // dim blue
static constexpr uint16_t C_EYE_HALO_2   = 0x02BF;  // medium blue
static constexpr uint16_t C_EYE_GLOW     = 0x04BF;  // bright cyan-blue
static constexpr uint16_t C_EYE_MID      = 0xBEFF;  // pale cyan
static constexpr uint16_t C_EYE_CORE     = 0xFFFF;  // white
static constexpr uint16_t C_EYE_CORE_ANG = 0xFFE0;  // yellow-white for angry
static constexpr uint16_t C_EYE_GLOW_ANG = 0xF800;  // red glow for angry
static constexpr uint16_t C_STATUS       = 0x04BF;

// ---------------------------------------------------------------------------
// Body silhouette — a union of circles centered around (120, 180), designed
// to look like a wide cloud / blob. Order matters only for the outline pass:
// bigger circles drawn later "swallow" the outline of earlier ones, which is
// exactly what we want to get a clean single silhouette.
// ---------------------------------------------------------------------------
struct Circ { int16_t x, y, r; };

static const Circ BODY_CIRCS[] = {
  // --- Top crown, between the two ears -----------------------
  {  95,  97, 26 },     // left shoulder of crown
  { 120,  87, 30 },     // central hump
  { 145,  97, 26 },     // right shoulder of crown

  // --- Upper torso --------------------------------------------
  {  65, 130, 38 },
  { 105, 123, 40 },
  { 135, 123, 40 },
  { 175, 130, 38 },

  // --- Mid section, the widest part ---------------------------
  {  52, 170, 42 },
  {  95, 170, 50 },
  { 145, 170, 50 },
  { 188, 170, 42 },

  // --- Bottom round -------------------------------------------
  {  72, 207, 44 },
  { 120, 215, 52 },
  { 168, 207, 44 },
};
static constexpr int N_BODY = sizeof(BODY_CIRCS) / sizeof(BODY_CIRCS[0]);

// Pointed ears — triangles sitting on top of the crown.
struct Tri { int16_t x0, y0, x1, y1, x2, y2; };
static const Tri EARS[] = {
  //  tip           inner-base      outer-base
  {  82,  49,      98,  93,         62,  93 },   // left ear
  { 158,  49,     178,  93,        142,  93 },   // right ear
};

// ---------------------------------------------------------------------------
// Face geometry — the rectangle that we re-blit every animation frame.
// Sized so the eye glow halo and the mouth both fit inside.
// ---------------------------------------------------------------------------
static constexpr int16_t FACE_X = 30;
static constexpr int16_t FACE_Y = 115;
static constexpr int16_t FACE_W = 180;
static constexpr int16_t FACE_H = 115;

// Eye centers, angles and base size. The reference image has "angry-inward"
// eyes: outer edges ride high, inner edges dip low.
static constexpr int16_t LEFT_EYE_CX  = 87;
static constexpr int16_t LEFT_EYE_CY  = 153;
static constexpr int16_t RIGHT_EYE_CX = 153;
static constexpr int16_t RIGHT_EYE_CY = 153;
static constexpr float   LEFT_EYE_ANGLE  =  0.34f;   // outer→inner slope down
static constexpr float   RIGHT_EYE_ANGLE = -0.34f;

static constexpr int16_t MOUTH_CX = 120;
static constexpr int16_t MOUTH_CY = 197;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static TFT_eSPI    *s_tft       = nullptr;
static TFT_eSprite *s_bodyFace  = nullptr;   // cached body inside FACE rect
static TFT_eSprite *s_faceBuf   = nullptr;   // per-frame output
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
static String       s_subPrefix;
static String       s_subText;
static String       s_lastSubDrawn = "\x01invalid\x01";

// Subtitle panel layout (directly below the creature).
static constexpr int16_t SUB_X = 0;
static constexpr int16_t SUB_Y = 272;
static constexpr int16_t SUB_W = 240;
static constexpr int16_t SUB_H = 48;
static constexpr uint16_t C_SUB_BG     = 0x0000;
static constexpr uint16_t C_SUB_PREFIX = 0x04BF;   // neon blue for "daemon:" / "you:"
static constexpr uint16_t C_SUB_TEXT   = 0xFFFF;

// ---------------------------------------------------------------------------
// Body painter — draws the neon-blue silhouette. Works on anything that
// exposes fillCircle / fillTriangle (TFT_eSPI or TFT_eSprite). Call
// `paintBody(target, dx, dy)` to translate the whole creature by (dx, dy).
// Two passes:
//   1. outline pass: draw the union of all primitives inflated by OUTLINE_W
//   2. interior pass: draw the same primitives at their real size in black
// The result is a clean, uniform, thick stroke around the body silhouette.
// ---------------------------------------------------------------------------
template <typename S>
static void paintBody(S *s, int16_t dx, int16_t dy) {
  // ---- Outline pass ----
  for (int i = 0; i < N_BODY; ++i) {
    const Circ &c = BODY_CIRCS[i];
    s->fillCircle(c.x + dx, c.y + dy, c.r + OUTLINE_W, C_OUTLINE);
  }
  for (int i = 0; i < 2; ++i) {
    const Tri &t = EARS[i];
    // Compute centroid, then push each vertex outward by OUTLINE_W along
    // its own radial direction so the expanded triangle "fattens" evenly.
    float cx = (t.x0 + t.x1 + t.x2) / 3.0f;
    float cy = (t.y0 + t.y1 + t.y2) / 3.0f;
    auto grow = [&](int x, int y, int &ox, int &oy) {
      float vx = x - cx, vy = y - cy;
      float len = sqrtf(vx * vx + vy * vy);
      if (len < 1.0f) { ox = x; oy = y; return; }
      ox = x + (int)(vx * OUTLINE_W / len);
      oy = y + (int)(vy * OUTLINE_W / len);
    };
    int x0, y0, x1, y1, x2, y2;
    grow(t.x0, t.y0, x0, y0);
    grow(t.x1, t.y1, x1, y1);
    grow(t.x2, t.y2, x2, y2);
    s->fillTriangle(x0 + dx, y0 + dy, x1 + dx, y1 + dy, x2 + dx, y2 + dy, C_OUTLINE);
  }

  // ---- Interior pass ----
  for (int i = 0; i < N_BODY; ++i) {
    const Circ &c = BODY_CIRCS[i];
    s->fillCircle(c.x + dx, c.y + dy, c.r, C_BODY);
  }
  for (int i = 0; i < 2; ++i) {
    const Tri &t = EARS[i];
    s->fillTriangle(t.x0 + dx, t.y0 + dy,
                    t.x1 + dx, t.y1 + dy,
                    t.x2 + dx, t.y2 + dy, C_BODY);
  }
}

// ---------------------------------------------------------------------------
// Eye — a tapered "pill" (almond). Drawn with several concentric passes to
// fake a soft glow halo around a bright cyan/white core.
// ---------------------------------------------------------------------------
static void drawEyePill(TFT_eSprite *s, int cx, int cy, float angle,
                        float halfLen, float thickness, uint16_t color) {
  int steps = max(14, (int)(halfLen * 2.0f));
  for (int i = 0; i <= steps; ++i) {
    float t   = (float)i / (float)steps;
    float off = (t - 0.5f) * 2.0f * halfLen;
    float tap = sinf(t * (float)PI);      // 0..1..0 taper
    int   r   = (int)(thickness * tap);
    if (r <= 0) continue;
    int   x   = cx + (int)(cosf(angle) * off);
    int   y   = cy + (int)(sinf(angle) * off);
    s->fillCircle(x, y, r, color);
  }
}

static void drawEye(TFT_eSprite *s, int cxScreen, int cyScreen, float angle,
                    float pulse, float blinkT, CreatureMood mood) {
  int cx = cxScreen - FACE_X;
  int cy = cyScreen - FACE_Y;

  // Base almond dimensions — match the reference's proportions: wide + bold.
  float baseHalfLen   = 28.0f;
  float baseThickness = 13.0f;

  if (mood == MOOD_LISTEN) baseThickness += 1.5f;
  if (mood == MOOD_HAPPY)  baseThickness *= 0.55f;   // squinty smile
  if (mood == MOOD_THINK)  baseThickness *= 0.9f;

  // Pulse: gentle swell (±12%)
  float scale = 1.0f + 0.12f * pulse;

  // Blink: smooth squeeze of the vertical thickness.
  if (blinkT >= 0.0f) {
    float c = 0.5f * (1.0f - cosf(blinkT * 2.0f * (float)PI));
    float closure = 1.0f - c;             // 0..1..0
    scale *= 0.18f + 0.82f * closure;
  }

  float halfLen   = baseHalfLen * (1.0f + 0.03f * pulse);
  float thickness = baseThickness * scale;

  // Layered glow halo.
  uint16_t cHalo0 = C_EYE_HALO_0;
  uint16_t cHalo1 = C_EYE_HALO_1;
  uint16_t cHalo2 = C_EYE_HALO_2;
  uint16_t cGlow  = (mood == MOOD_ANGRY) ? C_EYE_GLOW_ANG : C_EYE_GLOW;
  uint16_t cMid   = C_EYE_MID;
  uint16_t cCore  = (mood == MOOD_ANGRY) ? C_EYE_CORE_ANG : C_EYE_CORE;

  drawEyePill(s, cx, cy, angle, halfLen + 10.0f, thickness + 11.0f, cHalo0);
  drawEyePill(s, cx, cy, angle, halfLen + 6.0f,  thickness + 7.0f,  cHalo1);
  drawEyePill(s, cx, cy, angle, halfLen + 3.0f,  thickness + 4.0f,  cHalo2);
  drawEyePill(s, cx, cy, angle, halfLen,         thickness,         cGlow);
  drawEyePill(s, cx, cy, angle, halfLen * 0.82f, thickness * 0.72f, cMid);
  drawEyePill(s, cx, cy, angle, halfLen * 0.55f, thickness * 0.5f,  cCore);
}

// ---------------------------------------------------------------------------
// Mouth — an outlined capsule with a small tongue dot at the bottom. Shape
// opens vertically while talking.
// ---------------------------------------------------------------------------
static void drawMouth(TFT_eSprite *s, float openness, CreatureMood mood) {
  int cx = MOUTH_CX - FACE_X;
  int cy = MOUTH_CY - FACE_Y;

  if (openness < 0.0f) openness = 0.0f;
  if (openness > 1.0f) openness = 1.0f;

  if (mood == MOOD_HAPPY) {
    // Smile arc
    for (int dx = -14; dx <= 14; dx += 1) {
      int yy = cy + (int)(6.0f - (dx * dx) / 40.0f);
      s->fillCircle(cx + dx, yy, 2, C_OUTLINE);
    }
    return;
  }

  // Open-mouth capsule: a rectangle with rounded ends.
  int halfW = 12;
  int halfH = 2 + (int)(openness * 9.0f);

  // Outer blue rim (same stroke width as the body).
  s->fillCircle(cx - halfW, cy, halfH + OUTLINE_W - 1, C_OUTLINE);
  s->fillCircle(cx + halfW, cy, halfH + OUTLINE_W - 1, C_OUTLINE);
  s->fillRect(cx - halfW, cy - halfH - (OUTLINE_W - 1),
              halfW * 2, (halfH + (OUTLINE_W - 1)) * 2, C_OUTLINE);

  // Hollow the center in black.
  s->fillCircle(cx - halfW, cy, halfH, C_BODY);
  s->fillCircle(cx + halfW, cy, halfH, C_BODY);
  s->fillRect(cx - halfW, cy - halfH, halfW * 2, halfH * 2, C_BODY);

  // Little tongue blob hanging down from the bottom of the mouth.
  // Always present, rises with openness.
  int tongueY = cy + halfH - 1;
  int tongueR = 3 + (int)(openness * 2.0f);
  s->fillCircle(cx + 1, tongueY + tongueR / 2, tongueR,     C_OUTLINE);
  s->fillCircle(cx + 1, tongueY + tongueR / 2, tongueR - 2, 0x58A3); // dim blue fill
}

// ---------------------------------------------------------------------------
// Status bar on the top of the display.
// ---------------------------------------------------------------------------
static constexpr uint16_t C_PRICE = 0x07FF;   // near-cyan for the SOL ticker

static void drawStatusIfChanged(bool force) {
  bool statusChanged = s_status != s_lastStatusDrawn;
  bool priceChanged  = s_price  != s_lastPriceDrawn;
  if (!force && !statusChanged && !priceChanged) return;

  s_tft->fillRect(0, 0, SCR_W, STATUS_H, C_BG);
  s_tft->setTextFont(1);

  // LEFT: status
  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextColor(C_STATUS, C_BG);
  s_tft->setCursor(4, 4);
  s_tft->print(s_status);

  // RIGHT: price ticker
  if (s_price.length() > 0) {
    s_tft->setTextDatum(TR_DATUM);
    s_tft->setTextColor(C_PRICE, C_BG);
    s_tft->drawString(s_price, SCR_W - 4, 4);
  }

  s_lastStatusDrawn = s_status;
  s_lastPriceDrawn  = s_price;
}

// ---------------------------------------------------------------------------
// Subtitle renderer — word-wraps into up to 3 lines of Font 2 and draws the
// prefix in neon blue + the rest in white. Only repaints when the content
// changed so we don't flicker.
// ---------------------------------------------------------------------------
static void drawSubtitleIfChanged(bool force) {
  String combined = s_subPrefix + "\x01" + s_subText;
  if (!force && combined == s_lastSubDrawn) return;
  s_lastSubDrawn = combined;

  s_tft->fillRect(SUB_X, SUB_Y, SUB_W, SUB_H, C_SUB_BG);
  if (s_subText.length() == 0 && s_subPrefix.length() == 0) return;

  s_tft->setTextDatum(TL_DATUM);
  s_tft->setTextFont(2);
  const int lineH = 16;
  const int maxLines = (SUB_H / lineH);   // 3

  // Cursor start
  int x = SUB_X + 4;
  int y = SUB_Y + 2;

  if (s_subPrefix.length() > 0) {
    s_tft->setTextColor(C_SUB_PREFIX, C_SUB_BG);
    s_tft->setCursor(x, y);
    s_tft->print(s_subPrefix);
    x += s_tft->textWidth(s_subPrefix) + 4;
  }

  s_tft->setTextColor(C_SUB_TEXT, C_SUB_BG);

  // Word-wrap greedily: build a line, if adding the next word would spill,
  // flush the line and drop to the next. If we run out of lines, truncate
  // with an ellipsis.
  String remaining = s_subText;
  int line = 0;
  while (remaining.length() > 0 && line < maxLines) {
    int maxW = (SUB_X + SUB_W) - x - 4;
    String current = "";
    while (remaining.length() > 0) {
      int sp = remaining.indexOf(' ');
      String word = (sp < 0) ? remaining : remaining.substring(0, sp);
      String tryLine = current.length() ? current + " " + word : word;
      if (s_tft->textWidth(tryLine) > maxW) {
        if (current.length() == 0) {
          // Single word too long — hard-break it.
          while (word.length() > 0 &&
                 s_tft->textWidth(word) > maxW) {
            word.remove(word.length() - 1);
          }
          current = word;
          // Skip the clipped remainder of that word on the source side.
          int consumed = word.length();
          remaining = (sp < 0) ? "" : remaining.substring(sp + 1);
          if (sp < 0 && consumed < word.length()) { /* no-op */ }
        }
        break;
      }
      current = tryLine;
      remaining = (sp < 0) ? "" : remaining.substring(sp + 1);
    }
    // Truncation on last line if still more text left.
    if (line == maxLines - 1 && remaining.length() > 0) {
      while (current.length() > 0 &&
             s_tft->textWidth(current + "...") > ((SUB_X + SUB_W) - x - 4)) {
        current.remove(current.length() - 1);
      }
      current += "...";
    }
    s_tft->setCursor(x, y);
    s_tft->print(current);
    y += lineH;
    line++;
    x = SUB_X + 4;   // subsequent lines start at left margin, no prefix indent
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool creatureBegin(TFT_eSPI *tft) {
  s_tft = tft;
  tft->fillScreen(C_BG);
  paintBody(tft, (int16_t)0, (int16_t)0);

  // Face background — cached copy of the body pixels inside the animated
  // area, used to cleanly "erase" the previous frame's eyes/mouth.
  s_bodyFace = new TFT_eSprite(tft);
  s_bodyFace->setColorDepth(16);
  s_bodyFace->setAttribute(PSRAM_ENABLE, true);
  if (!s_bodyFace->createSprite(FACE_W, FACE_H)) {
    Serial.println("creature: bodyFace sprite alloc failed");
    return false;
  }
  s_bodyFace->fillSprite(C_BG);
  paintBody(s_bodyFace, (int16_t)-FACE_X, (int16_t)-FACE_Y);

  s_faceBuf = new TFT_eSprite(tft);
  s_faceBuf->setColorDepth(16);
  s_faceBuf->setAttribute(PSRAM_ENABLE, true);
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

void creatureSetMood(CreatureMood m)    { s_mood = m; }
void creatureSetTalking(bool on)        { s_talking = on; if (!on) s_mouthEnv = 0.0f; }
void creatureForceBlink()               { s_forceBlink = true; }
void creatureSetStatus(const String &s) { s_status = s; }
void creatureSetPrice (const String &s) { s_price  = s; }
void creatureSetSubtitle(const String &prefix, const String &text) {
  s_subPrefix = prefix;
  s_subText   = text;
}

void creatureTick() {
  if (!s_faceBuf || !s_bodyFace) return;

  uint32_t now = millis();
  float dt = (now - s_lastTickMs) / 1000.0f;
  if (dt > 0.25f) dt = 0.25f;
  s_lastTickMs = now;

  s_animPhase += dt * (s_mood == MOOD_LISTEN ? 2.4f : 1.5f);
  if (s_animPhase > 2.0f * (float)PI) s_animPhase -= 2.0f * (float)PI;
  float pulse = sinf(s_animPhase);

  // Blink cycle.
  float blinkT = -1.0f;
  if (s_forceBlink && now - s_blinkStartMs > 400) {
    s_blinkStartMs = now;
    s_forceBlink = false;
  }
  bool blinking = (now - s_blinkStartMs) < 180;
  if (blinking) {
    blinkT = (now - s_blinkStartMs) / 180.0f;
  } else if (now - s_lastBlinkMs > (uint32_t)random(3000, 6000)) {
    s_blinkStartMs = now;
    s_lastBlinkMs = now;
  }

  // Mouth drive.
  float targetOpen = 0.0f;
  if (s_talking || s_mood == MOOD_TALK) {
    s_mouthPhase += dt * 13.0f;
    float a = 0.5f * (1.0f + sinf(s_mouthPhase));
    float b = 0.5f * (1.0f + sinf(s_mouthPhase * 0.37f + 1.1f));
    targetOpen = a * 0.7f + b * 0.3f;
    if (((int)(s_mouthPhase * 0.3f)) % 7 == 0) targetOpen *= 0.3f;
  }
  s_mouthEnv += (targetOpen - s_mouthEnv) * min(1.0f, dt * 18.0f);

  // Render frame: body-cache → frame-buffer, then features on top.
  s_bodyFace->pushToSprite(s_faceBuf, 0, 0);
  drawEye(s_faceBuf, LEFT_EYE_CX,  LEFT_EYE_CY,  LEFT_EYE_ANGLE,
          pulse, blinkT, s_mood);
  drawEye(s_faceBuf, RIGHT_EYE_CX, RIGHT_EYE_CY, RIGHT_EYE_ANGLE,
          pulse, blinkT, s_mood);
  drawMouth(s_faceBuf, s_mouthEnv, s_mood);
  s_faceBuf->pushSprite(FACE_X, FACE_Y);

  drawStatusIfChanged(false);
  drawSubtitleIfChanged(false);
}
