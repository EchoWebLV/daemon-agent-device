#include "creature.h"
#include "statusicons.h"
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
static uint8_t      s_faceStyle = 0;         // 0=Daemon, 1=Robot, 2=ToyBot, 3=Calc
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
// FACE STYLE 1 — ROBOT
// Big circular eyes with a glowing halo + a smaller dark pupil that drifts
// with the gaze. The mouth is a thin rounded bar that opens vertically
// when talking. Reads as a classic friendly bot.
// ---------------------------------------------------------------------------
static void drawEyeRobot(TFT_eSprite *s, int cxScreen, int cyScreen,
                         float pulse, float blinkT, CreatureMood mood) {
  int cx = cxScreen - FACE_X;
  int cy = cyScreen - FACE_Y;

  // Outer eye radius. Pulse scales ±5% so the idle glow "breathes".
  float rOuter = 30.0f * (1.0f + 0.05f * pulse);
  float vScale = 1.0f;

  if (blinkT >= 0.0f) {
    float c = 0.5f * (1.0f - cosf(blinkT * 2.0f * (float)PI));
    vScale = 0.1f + 0.9f * (1.0f - c);
  }

  uint16_t cGlow = (mood == MOOD_ANGRY) ? C_GLOW_ANG : C_ACCENT;
  uint16_t cCore = (mood == MOOD_ANGRY) ? C_CORE_ANG : C_CORE;

  // Five concentric ellipses (fake ellipse via cheap y-compression) —
  // outermost halo → core. vScale squashes everything vertically for blinks.
  auto squashedCircle = [&](int r, uint16_t color) {
    int ry = max(1, (int)(r * vScale));
    for (int dy = -ry; dy <= ry; ++dy) {
      float t = (float)dy / (float)ry;
      int dx = (int)(r * sqrtf(max(0.0f, 1.0f - t * t)));
      s->drawFastHLine(cx - dx, cy + dy, dx * 2 + 1, color);
    }
  };

  squashedCircle((int)(rOuter + 10), C_HALO_0);
  squashedCircle((int)(rOuter + 6),  C_HALO_1);
  squashedCircle((int)(rOuter + 3),  C_HALO_2);
  squashedCircle((int)rOuter,        cGlow);
  squashedCircle((int)(rOuter * 0.72f), C_MID);
  squashedCircle((int)(rOuter * 0.48f), cCore);

  // Pupil — small dark dot that drifts slightly with the current gaze.
  // Gaze is already applied to the screen coordinates passed in, so we
  // add a tiny extra offset for "looking around" parallax.
  if (vScale > 0.25f) {
    int pr = 6;
    s->fillCircle(cx, cy, pr, C_BG);
    s->fillCircle(cx - 1, cy - 1, 2, cCore);     // specular highlight
  }
}

static void drawMouthRobot(TFT_eSprite *s, float openness, CreatureMood mood,
                           int yOffset) {
  int cx = MOUTH_CX - FACE_X;
  int cy = MOUTH_CY - FACE_Y + yOffset;

  if (openness < 0.0f) openness = 0.0f;
  if (openness > 1.0f) openness = 1.0f;

  if (mood == MOOD_HAPPY) {
    // Upward smile — wider and thinner than the Daemon arc.
    for (int dx = -28; dx <= 28; dx += 1) {
      int yy = cy + (int)(8.0f - (dx * dx) / 110.0f);
      s->fillCircle(cx + dx, yy, 2, C_ACCENT);
    }
    return;
  }

  // Base horizontal bar. Grows vertically with `openness`.
  int halfW = 30;
  int halfH = 2 + (int)(openness * 10.0f);
  s->fillRoundRect(cx - halfW, cy - halfH, halfW * 2, halfH * 2,
                   halfH, C_ACCENT);

  // Inner highlight stripe — gives the bar a subtle "speaker grille" feel.
  if (halfH > 3) {
    s->drawFastHLine(cx - halfW + 6, cy, (halfW - 6) * 2 + 1, C_CORE);
  }
}

// ---------------------------------------------------------------------------
// FACE STYLE 2 — TOY ROBOT
// Cute squarish blue robot head with a glowing red antenna bulb, diagonal
// shine stripes, four corner rivets, two round red eyes with black pupils,
// and a yellow speaker-grille mouth that animates when talking.
//
// Drawn in three layers: head chassis (frame + antenna + rivets + shine),
// then eyes, then mouth — so the animated features sit on top of the
// static chassis.
// ---------------------------------------------------------------------------
static constexpr uint16_t C_TR_BODY     = 0x5DDB;   // light cyan-blue fill
static constexpr uint16_t C_TR_SHINE    = 0xC75F;   // pale cyan sheen stripe
static constexpr uint16_t C_TR_RIM      = 0x1B6D;   // darker blue rim / rivets
static constexpr uint16_t C_TR_EYE      = 0xF800;   // red eye
static constexpr uint16_t C_TR_EYE_DK   = 0x9800;   // dark red eye rim
static constexpr uint16_t C_TR_EYE_HL   = 0xFFFF;   // eye highlight speckle
static constexpr uint16_t C_TR_PUPIL    = 0x0000;   // black pupil
static constexpr uint16_t C_TR_MOUTH_BG = 0xFCE0;   // orange-yellow fill
static constexpr uint16_t C_TR_MOUTH_BAR= 0x2965;   // dark grille bars
static constexpr uint16_t C_TR_ANT_BULB = 0xF800;
static constexpr uint16_t C_TR_ANT_DK   = 0x9800;
static constexpr uint16_t C_TR_STALK    = 0xA514;   // light grey stalk
static constexpr uint16_t C_TR_SPARK    = 0xFFFF;   // motion / "on" lines

// Head geometry in sprite coordinates. Sprite origin (0,0) == screen
// (FACE_X, FACE_Y). The head encloses both eyes (sprite cy≈85) and the
// mouth (sprite cy≈155) with room above for the antenna.
static constexpr int TR_HEAD_CX = 120;
static constexpr int TR_HEAD_CY = 122;
static constexpr int TR_HEAD_W  = 178;
static constexpr int TR_HEAD_H  = 158;
static constexpr int TR_HEAD_R  = 20;

static void drawHeadToyRobot(TFT_eSprite *s, int bobDY, CreatureMood mood,
                             float pulse) {
  const int cx = TR_HEAD_CX;
  const int cy = TR_HEAD_CY + bobDY;
  const int hx = cx - TR_HEAD_W / 2;
  const int hy = cy - TR_HEAD_H / 2;

  // ── Antenna ──
  // Stalk rises from the top-centre of the head. Bulb pulses ±2 px with
  // the idle breathing so the robot reads as powered-on.
  const int stalkTop = hy - 20;
  const int stalkBot = hy + 1;
  s->drawFastVLine(cx,     stalkTop, stalkBot - stalkTop, C_TR_STALK);
  s->drawFastVLine(cx + 1, stalkTop, stalkBot - stalkTop, C_TR_STALK);

  int bulbR = 7 + (int)(1.5f * (pulse > 0.0f ? pulse : 0.0f));
  int bulbCy = stalkTop - bulbR - 1;
  s->fillCircle(cx, bulbCy, bulbR + 1, C_TR_ANT_DK);
  s->fillCircle(cx, bulbCy, bulbR,     C_TR_ANT_BULB);
  s->fillCircle(cx - 2, bulbCy - 2, 2, C_TR_EYE_HL);   // shiny speck

  // Sparkle starburst around the bulb (6 radial ticks). Hidden when
  // angry; otherwise always on so the antenna always looks "live".
  if (mood != MOOD_ANGRY) {
    const int N = 6;
    for (int i = 0; i < N; ++i) {
      float a = (float)i * (2.0f * (float)PI / (float)N) - 0.5f * (float)PI;
      int r0 = bulbR + 4;
      int r1 = bulbR + ((i % 2 == 0) ? 10 : 7);
      int x0 = cx + (int)(cosf(a) * r0);
      int y0 = bulbCy + (int)(sinf(a) * r0);
      int x1 = cx + (int)(cosf(a) * r1);
      int y1 = bulbCy + (int)(sinf(a) * r1);
      s->drawLine(x0, y0, x1, y1, C_TR_SPARK);
    }
  }

  // ── Head body ──
  // 2-px darker rim drawn behind the body fill for an outline effect
  // against the black background (we can't use a black outline — that's
  // the sprite bg colour).
  s->fillRoundRect(hx - 2, hy - 2, TR_HEAD_W + 4, TR_HEAD_H + 4,
                   TR_HEAD_R + 2, C_TR_RIM);
  s->fillRoundRect(hx,     hy,     TR_HEAD_W,     TR_HEAD_H,
                   TR_HEAD_R,      C_TR_BODY);

  // Diagonal shine stripes. Each scanline draws a horizontal band whose
  // x-offset slides with y so the whole band reads as a parallelogram
  // tilted right. Clipped to a rounded rect so it respects the head shape.
  auto drawStripe = [&](int startX, int w) {
    for (int dy = 0; dy < TR_HEAD_H; ++dy) {
      int y = hy + dy;
      int xs = hx + startX + dy / 2;
      int xe = xs + w;
      int edgeDist = min(dy, TR_HEAD_H - 1 - dy);
      int inset = (edgeDist < TR_HEAD_R)
                  ? (int)(TR_HEAD_R - sqrtf((float)(TR_HEAD_R * TR_HEAD_R) -
                                            (float)((TR_HEAD_R - edgeDist) *
                                                    (TR_HEAD_R - edgeDist))))
                  : 0;
      int clipL = hx + inset;
      int clipR = hx + TR_HEAD_W - inset;
      if (xs < clipL) xs = clipL;
      if (xe > clipR) xe = clipR;
      if (xe > xs) s->drawFastHLine(xs, y, xe - xs, C_TR_SHINE);
    }
  };
  drawStripe(34, 18);
  drawStripe(96, 10);

  // ── Rivets at each corner ──
  const int rvIn = 14;
  const int rvs[4][2] = {
    { hx + rvIn,              hy + rvIn              },
    { hx + TR_HEAD_W - rvIn,  hy + rvIn              },
    { hx + rvIn,              hy + TR_HEAD_H - rvIn  },
    { hx + TR_HEAD_W - rvIn,  hy + TR_HEAD_H - rvIn  },
  };
  for (const auto &r : rvs) {
    s->fillCircle(r[0], r[1], 3, C_TR_RIM);
    s->drawPixel(r[0] - 1, r[1] - 1, C_TR_EYE_HL);
  }
}

static void drawEyeToyRobot(TFT_eSprite *s, int cxScreen, int cyScreen,
                            float pulse, float blinkT, CreatureMood mood) {
  int cx = cxScreen - FACE_X;
  int cy = cyScreen - FACE_Y;

  int r = 15 + (int)(1.0f * pulse);
  if (mood == MOOD_LISTEN) r += 2;
  if (mood == MOOD_THINK)  r -= 1;

  uint16_t col   = (mood == MOOD_ANGRY) ? 0xFB40 : C_TR_EYE;
  uint16_t colDk = C_TR_EYE_DK;

  // Happy → curved squinty arc of red dots, like a smile for the eyes.
  if (mood == MOOD_HAPPY) {
    for (int dx = -12; dx <= 12; ++dx) {
      int yy = cy + 3 - (int)((144 - dx * dx) / 28);
      s->fillCircle(cx + dx, yy, 3, col);
    }
    return;
  }

  // Blink — squash vertically. Near full closure we render a thin red
  // slit (eyelid closing on a glow) rather than a dot.
  if (blinkT >= 0.0f) {
    float c = 0.5f * (1.0f - cosf(blinkT * 2.0f * (float)PI));
    float closure = 1.0f - c;
    int rh = max(1, (int)(r * (0.1f + 0.9f * closure)));
    for (int dy = -rh; dy <= rh; ++dy) {
      float t = (float)dy / (float)rh;
      int dx = (int)(r * sqrtf(max(0.0f, 1.0f - t * t)));
      s->drawFastHLine(cx - dx, cy + dy, dx * 2 + 1, col);
    }
    if (closure > 0.45f) {
      int pr = max(1, (int)(4.0f * closure));
      s->fillCircle(cx, cy, pr, C_TR_PUPIL);
    }
    return;
  }

  // Default open eye: dark rim + red disc + black pupil + white speck.
  s->fillCircle(cx, cy, r + 1, colDk);
  s->fillCircle(cx, cy, r,     col);
  s->fillCircle(cx, cy, 4,     C_TR_PUPIL);
  s->fillCircle(cx - r / 2 + 1, cy - r / 2 + 1, 2, C_TR_EYE_HL);
}

// ---------------------------------------------------------------------------
// FACE STYLE 3 — CALCULATOR (minimal `| |` + `—` mascot face)
// Two small vertical pills for eyes and a thin horizontal capsule for the
// mouth, all rendered in the same crisp accent colour as the rest of the
// chrome. Intentionally minimalist to match the reference sketch — no
// halo, no 7-segment ghosting, just two ovals and a line on a black
// background.
// ---------------------------------------------------------------------------
static void drawCalcPill(TFT_eSprite *s, int cx, int cy,
                         int halfW, int halfH, uint16_t color) {
  // Rounded capsule: a filled rectangle bookended by two filled circles.
  // Works for both orientations depending on whether halfW < halfH (vertical
  // pill) or halfW > halfH (horizontal pill).
  if (halfW <= 0 || halfH <= 0) return;
  if (halfH >= halfW) {
    // Vertical pill — short axis is width.
    s->fillRect(cx - halfW, cy - halfH + halfW,
                halfW * 2, max(0, (halfH - halfW) * 2), color);
    s->fillCircle(cx, cy - halfH + halfW, halfW, color);
    s->fillCircle(cx, cy + halfH - halfW, halfW, color);
  } else {
    // Horizontal pill — short axis is height.
    s->fillRect(cx - halfW + halfH, cy - halfH,
                max(0, (halfW - halfH) * 2), halfH * 2, color);
    s->fillCircle(cx - halfW + halfH, cy, halfH, color);
    s->fillCircle(cx + halfW - halfH, cy, halfH, color);
  }
}

static void drawEyeCalculator(TFT_eSprite *s, int cxScreen, int cyScreen,
                              float pulse, float blinkT, CreatureMood mood) {
  int cx = cxScreen - FACE_X;
  int cy = cyScreen - FACE_Y;

  // Base dimensions: a small vertical pill, ~10 wide × ~36 tall, which
  // matches the proportions in the reference sketch.
  int halfW = 6;
  int halfH = 18;

  // Mood tweaks. Happy squints the pill shorter; angry lengthens it and
  // tilts the top corner slightly inward (we fake that via a horizontal
  // offset between the two eyes, handled by the gaze system).
  if (mood == MOOD_HAPPY)  halfH = 10;
  if (mood == MOOD_LISTEN) halfH = 22;
  if (mood == MOOD_ANGRY)  halfH = 22;

  // Subtle breathing swell — ±5% on the tall axis, almost imperceptible
  // but gives the pills a hint of "alive".
  halfH = (int)(halfH * (1.0f + 0.05f * pulse));

  // Blink transitions the vertical pill into a short horizontal dash and
  // back. cos-interp makes the mid-point a flat line, which is the
  // classic `-` closed-eye look.
  if (blinkT >= 0.0f) {
    float c = 0.5f * (1.0f - cosf(blinkT * 2.0f * (float)PI));
    halfH = max(3, (int)(halfH * (1.0f - 0.9f * c)));
    // Swap to a horizontal dash when fully closed — just widen slightly so
    // the closed state reads as `—` rather than a dot.
    if (c > 0.7f) {
      int dashW = 10;
      int dashH = 3;
      uint16_t col = (mood == MOOD_ANGRY) ? C_CORE_ANG : C_CORE;
      drawCalcPill(s, cx, cy, dashW, dashH, col);
      return;
    }
  }

  uint16_t col = (mood == MOOD_ANGRY) ? C_CORE_ANG : C_CORE;
  drawCalcPill(s, cx, cy, halfW, halfH, col);
}

static void drawMouthCalculator(TFT_eSprite *s, float openness,
                                CreatureMood mood, int yOffset) {
  int cx = MOUTH_CX - FACE_X;
  int cy = MOUTH_CY - FACE_Y + yOffset;

  if (openness < 0.0f) openness = 0.0f;
  if (openness > 1.0f) openness = 1.0f;

  uint16_t col = (mood == MOOD_ANGRY) ? C_CORE_ANG : C_CORE;

  if (mood == MOOD_HAPPY) {
    // Tiny upward arc for a content smile.
    for (int dx = -18; dx <= 18; ++dx) {
      int yy = cy + (int)(6.0f - (dx * dx) / 60.0f);
      s->drawPixel(cx + dx,     yy,     col);
      s->drawPixel(cx + dx,     yy + 1, col);
      s->drawPixel(cx + dx,     yy - 1, col);
    }
    return;
  }

  // Thin horizontal line mouth. Base is 2 px tall; talking grows it up to
  // ~6 px so the line "fattens" rhythmically as Daemon speaks.
  int halfW = 18;
  int halfH = 1 + (int)(openness * 3.0f);
  drawCalcPill(s, cx, cy, halfW, halfH, col);
}

static void drawMouthToyRobot(TFT_eSprite *s, float openness, CreatureMood mood,
                              int yOffset) {
  int cx = MOUTH_CX - FACE_X;
  int cy = MOUTH_CY - FACE_Y + yOffset;

  if (openness < 0.0f) openness = 0.0f;
  if (openness > 1.0f) openness = 1.0f;

  if (mood == MOOD_HAPPY) {
    // Orange upward grin arc. Drawn as a thick band of filled dots so it
    // stands out against the blue head chassis behind it.
    for (int dx = -22; dx <= 22; ++dx) {
      int yy = cy + 6 - (int)((484 - dx * dx) / 70);
      s->fillCircle(cx + dx, yy, 3, C_TR_MOUTH_BG);
    }
    return;
  }

  // Base grille: orange-yellow rounded rect with 5 vertical dark bars.
  // Height grows with `openness` so the whole panel "breathes" while
  // talking.
  const int halfW = 30;
  const int halfH = 7 + (int)(openness * 9.0f);

  s->fillRoundRect(cx - halfW - 1, cy - halfH - 1,
                   (halfW + 1) * 2, (halfH + 1) * 2, 5, C_TR_RIM);
  s->fillRoundRect(cx - halfW,     cy - halfH,
                   halfW * 2,       halfH * 2,       4, C_TR_MOUTH_BG);

  const int bars    = 5;
  const int barW    = 3;
  const int innerW  = halfW * 2 - 8;
  const int spacing = (bars > 1) ? (innerW - barW) / (bars - 1) : 0;
  const int barY    = cy - halfH + 2;
  const int barH    = halfH * 2 - 4;
  for (int i = 0; i < bars; ++i) {
    int bx = cx - halfW + 4 + i * spacing;
    if (barH > 0) s->fillRect(bx, barY, barW, barH, C_TR_MOUTH_BAR);
  }
}

// ---------------------------------------------------------------------------
// Status bar (left: status text, right: price ticker)
// ---------------------------------------------------------------------------
static void drawStatusIfChanged(bool force) {
  bool statusChanged = s_status != s_lastStatusDrawn;
  bool priceChanged  = s_price  != s_lastPriceDrawn;
  bool iconsChanged  = statusIconsNeedRedraw();
  if (!force && !statusChanged && !priceChanged && !iconsChanged) return;

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

  // Feature indicators sit centred between the USDC balance (left) and
  // the SOL ticker (right). They appear only when their respective toggle
  // is on, and disappear the instant the user flips it off.
  statusIconsDraw(s_tft, SCR_W / 2, STATUS_H / 2, C_ACCENT, C_BG);

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
  statusIconsResetCache();
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

void creatureSetFaceStyle(uint8_t style) {
  if (style > 3) style = 0;
  if (style == s_faceStyle) return;
  s_faceStyle = style;
  // Face geometry is identical across styles, so a cheap sprite repaint
  // on the next tick is enough — no full-screen redraw needed.
  if (s_faceBuf) s_faceBuf->fillSprite(0x0000);
}

uint8_t creatureFaceStyle() { return s_faceStyle; }

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
  switch (s_faceStyle) {
    case 1:   // Robot
      drawEyeRobot(s_faceBuf, leftX,  eyeY, pulse, blinkL, s_mood);
      drawEyeRobot(s_faceBuf, rightX, eyeY, pulse, blinkR, s_mood);
      drawMouthRobot(s_faceBuf, s_mouthEnv, s_mood, bobDY);
      break;
    case 2:   // Toy Robot — cute blue head with red antenna + grille mouth
      drawHeadToyRobot(s_faceBuf, bobDY, s_mood, pulse);
      drawEyeToyRobot(s_faceBuf, leftX,  eyeY, pulse, blinkL, s_mood);
      drawEyeToyRobot(s_faceBuf, rightX, eyeY, pulse, blinkR, s_mood);
      drawMouthToyRobot(s_faceBuf, s_mouthEnv, s_mood, bobDY);
      break;
    case 3:   // Calculator — =  |  analog LCD
      drawEyeCalculator(s_faceBuf, leftX,  eyeY, pulse, blinkL, s_mood);
      drawEyeCalculator(s_faceBuf, rightX, eyeY, pulse, blinkR, s_mood);
      drawMouthCalculator(s_faceBuf, s_mouthEnv, s_mood, bobDY);
      break;
    default:  // 0 = Daemon
      drawEye(s_faceBuf, leftX,  eyeY, LEFT_EYE_ANGLE,  pulse, blinkL, s_mood);
      drawEye(s_faceBuf, rightX, eyeY, RIGHT_EYE_ANGLE, pulse, blinkR, s_mood);
      drawMouth(s_faceBuf, s_mouthEnv, s_mood, bobDY);
      break;
  }
  s_faceBuf->pushSprite(FACE_X, FACE_Y);

  drawStatusIfChanged(false);
  tickSubtitleScroll(now);
  drawSubtitleIfChanged(false);
}
