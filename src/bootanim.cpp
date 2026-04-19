#include "bootanim.h"
#include <esp_system.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Geometry + palette
// ---------------------------------------------------------------------------
static constexpr int16_t SCR_W = 240;
static constexpr int16_t SCR_H = 320;
static constexpr int16_t CX    = 120;
static constexpr int16_t CY    = 160;

// Brand cyan — same #0F5DFD as the creature renderer, so transitions
// feel visually cohesive. Error tints reuse standard RGB565 values.
static constexpr uint16_t C_ACCENT    = 0x0AFF;   // cyan
static constexpr uint16_t C_RED       = 0xF800;
static constexpr uint16_t C_AMBER     = 0xFD20;
static constexpr uint16_t C_ORANGE    = 0xFBE0;
static constexpr uint16_t C_WHITE     = 0xFFFF;
static constexpr uint16_t C_BG        = 0x0000;

// ---------------------------------------------------------------------------
// Shared PSRAM sprite
//
// One 240×320×16bpp full-screen buffer. Allocated by bootAnimPlayPowerOn
// (first function called in boot order), kept alive across the rest of
// setup() so the wifi-join animation can reuse it, freed by
// wifiJoinAnimEnd. Living in PSRAM keeps internal DRAM free for the
// creature face sprite + task stacks + TLS buffers.
// ---------------------------------------------------------------------------
static TFT_eSprite *s_sprite = nullptr;
static TFT_eSPI    *s_tft    = nullptr;

static bool ensureSprite(TFT_eSPI *tft) {
  if (s_sprite) return true;
  if (!tft) return false;
  s_tft    = tft;
  s_sprite = new TFT_eSprite(tft);
  s_sprite->setColorDepth(16);
  // PSRAM_ENABLE = 3 (see transition.cpp); otherwise TFT_eSPI would try
  // to alloc 150 KB in internal DRAM and starve the face sprite.
  s_sprite->setAttribute(3 /*PSRAM_ENABLE*/, 1);
  if (!s_sprite->createSprite(SCR_W, SCR_H)) {
    Serial.println("bootanim: sprite alloc FAILED — animations disabled");
    delete s_sprite; s_sprite = nullptr;
    return false;
  }
  // Same swap-bytes requirement as the transition sprites.
  s_sprite->setSwapBytes(true);
  return true;
}

static void freeSprite() {
  if (!s_sprite) return;
  s_sprite->deleteSprite();
  delete s_sprite;
  s_sprite = nullptr;
}

// ---------------------------------------------------------------------------
// Math helpers
// ---------------------------------------------------------------------------
static inline float clamp01f(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float easeOut (float t) { float u = 1.0f - t; return 1.0f - u * u; }
static inline float easeInOut(float t) {
  return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) / 2.0f;
}

// Scale an RGB565 color toward black by `t` (0..1). Channel-wise
// multiplication with rounding; `t=1` returns the original, `t=0` is
// black. Used everywhere we want "dim version of the accent color".
static uint16_t dim(uint16_t color, float t) {
  if (t >= 1.0f) return color;
  if (t <= 0.0f) return 0;
  uint8_t r5 = (color >> 11) & 0x1F;
  uint8_t g6 = (color >>  5) & 0x3F;
  uint8_t b5 =  color        & 0x1F;
  uint8_t R = (uint8_t)(r5 * t + 0.5f);
  uint8_t G = (uint8_t)(g6 * t + 0.5f);
  uint8_t B = (uint8_t)(b5 * t + 0.5f);
  return (uint16_t)((R << 11) | (G << 5) | B);
}

// Simple frame pacer. Keeps the animation loop at ~30 FPS without
// over-sleeping when the previous frame took a long time to render.
static void paceFrame(uint32_t &lastFrameMs, uint32_t targetMs = 33) {
  uint32_t now = millis();
  uint32_t took = now - lastFrameMs;
  if (took < targetMs) delay(targetMs - took);
  lastFrameMs = millis();
}

// ---------------------------------------------------------------------------
// CRT power-on animation
//
// Five phases, shared timeline:
//   P1  horizontal scanline grows from center outward
//   P2  scanline pulses bright (white core), small vertical halo forms
//   P3  band stretches vertically to fill the screen; bright "wavefront"
//       edges on top + bottom leave a dim interior trail
//   P4  near-white full-screen flash (~80 ms)
//   P5  fade back to black
//
// All five phase lengths are scaled by `totalMs / 820` so the error
// variants (panic / brownout / WDT) complete in a few hundred ms.
// ---------------------------------------------------------------------------
void bootAnimPlayPowerOn(TFT_eSPI *tft, int resetReason) {
  if (!ensureSprite(tft)) return;

  // Pick tint + total duration from the reset reason. Error states get
  // short, colored blips; clean boots get the full cinematic.
  uint16_t accent;
  uint32_t totalMs;
  bool     errorMode = false;
  switch (resetReason) {
    case ESP_RST_PANIC:    accent = C_RED;    totalMs = 260; errorMode = true; break;
    case ESP_RST_BROWNOUT: accent = C_AMBER;  totalMs = 340; errorMode = true; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      accent = C_ORANGE; totalMs = 320; errorMode = true; break;
    case ESP_RST_SW:       accent = C_ACCENT; totalMs = 440; break;   // short cyan
    case ESP_RST_DEEPSLEEP:accent = C_ACCENT; totalMs = 560; break;
    case ESP_RST_POWERON:
    default:               accent = C_ACCENT; totalMs = 820; break;   // cinematic
  }

  // Phase boundaries (absolute ms from anim start).
  const float s = (float)totalMs / 820.0f;
  const uint32_t p1End = (uint32_t)(220.0f * s);   // horizontal grow
  const uint32_t p2End = p1End + (uint32_t)(100.0f * s);   // pulse
  const uint32_t p3End = p2End + (uint32_t)(260.0f * s);   // vertical stretch
  const uint32_t p4End = p3End + (uint32_t)( 80.0f * s);   // flash
  const uint32_t p5End = p4End + (uint32_t)(160.0f * s);   // fade

  // Make sure the panel is black before we start compositing. If the
  // LCD driver was mid-push when we got reset, stray pixels might still
  // be on screen; clear unconditionally.
  tft->fillScreen(C_BG);

  uint32_t start    = millis();
  uint32_t frameMs  = millis();

  while (true) {
    uint32_t e = millis() - start;
    if (e >= p5End) break;

    s_sprite->fillSprite(C_BG);

    if (e < p1End) {
      // P1: horizontal line grows outward from screen center.
      float t = clamp01f((float)e / (float)p1End);
      int16_t w = (int16_t)(SCR_W * easeOut(t));
      if (w < 3) w = 3;
      int16_t x = CX - w / 2;
      // Soft halo ±2 px + bright core row. Reads as a scanline igniting.
      s_sprite->drawFastHLine(x, CY - 2, w, dim(accent, 0.12f));
      s_sprite->drawFastHLine(x, CY - 1, w, dim(accent, 0.45f));
      s_sprite->drawFastHLine(x, CY,     w, accent);
      s_sprite->drawFastHLine(x, CY + 1, w, dim(accent, 0.45f));
      s_sprite->drawFastHLine(x, CY + 2, w, dim(accent, 0.12f));

    } else if (e < p2End) {
      // P2: full-width pulse. Core flashes to white at the peak.
      float t  = clamp01f((float)(e - p1End) / (float)(p2End - p1End));
      float v  = 0.5f + 0.5f * sinf(t * (float)PI);     // 0→1→0
      uint16_t core = (v > 0.78f) ? C_WHITE : accent;
      s_sprite->drawFastHLine(0, CY - 3, SCR_W, dim(accent, v * 0.08f));
      s_sprite->drawFastHLine(0, CY - 2, SCR_W, dim(accent, v * 0.25f));
      s_sprite->drawFastHLine(0, CY - 1, SCR_W, dim(accent, v * 0.60f));
      s_sprite->drawFastHLine(0, CY,     SCR_W, core);
      s_sprite->drawFastHLine(0, CY + 1, SCR_W, dim(accent, v * 0.60f));
      s_sprite->drawFastHLine(0, CY + 2, SCR_W, dim(accent, v * 0.25f));
      s_sprite->drawFastHLine(0, CY + 3, SCR_W, dim(accent, v * 0.08f));

    } else if (e < p3End) {
      // P3: band stretches vertically. Bright edges on top + bottom;
      // a dim interior fill in between. Because we redraw the whole
      // band each frame, the "bright edge on last frame" becomes "dim
      // interior on this frame" — reading as a wavefront sweeping out
      // and leaving a glowing trail behind.
      float t   = clamp01f((float)(e - p2End) / (float)(p3End - p2End));
      int16_t h = (int16_t)(SCR_H * easeOut(t));
      if (h < 5) h = 5;
      int16_t y     = CY - h / 2;
      int16_t yEnd  = y + h;
      if (y    < 0)      y    = 0;
      if (yEnd > SCR_H)  yEnd = SCR_H;

      // Dim interior fill.
      s_sprite->fillRect(0, y, SCR_W, yEnd - y, dim(accent, 0.14f));
      // Outer + inner glow rows. Guarded so we don't draw off-sprite.
      if (y      < SCR_H)    s_sprite->drawFastHLine(0, y,      SCR_W, accent);
      if (yEnd-1 >= 0)       s_sprite->drawFastHLine(0, yEnd-1, SCR_W, accent);
      if (y - 1  >= 0)       s_sprite->drawFastHLine(0, y - 1,  SCR_W, dim(accent, 0.55f));
      if (yEnd   <  SCR_H)   s_sprite->drawFastHLine(0, yEnd,   SCR_W, dim(accent, 0.55f));
      if (y - 2  >= 0)       s_sprite->drawFastHLine(0, y - 2,  SCR_W, dim(accent, 0.18f));
      if (yEnd+1 <  SCR_H)   s_sprite->drawFastHLine(0, yEnd+1, SCR_W, dim(accent, 0.18f));

    } else if (e < p4End) {
      // P4: near-white flash. Brief, unconditional full-screen fill.
      // For error tints we stop short of full white so the color reads
      // clearly (red flash / amber flash rather than "everything white").
      float t  = clamp01f((float)(e - p3End) / (float)(p4End - p3End));
      float a  = (t < 0.5f) ? t * 2.0f : (1.0f - (t - 0.5f) * 2.0f);
      if (errorMode) {
        s_sprite->fillSprite(dim(accent, 0.55f + 0.45f * a));
      } else {
        // Blend accent→white toward peak.
        uint8_t ra = (uint8_t)(31 * (0.1f + 0.9f * a));
        uint8_t ga = (uint8_t)(63 * (0.1f + 0.9f * a));
        uint8_t ba = (uint8_t)(31 * (0.1f + 0.9f * a));
        uint16_t col = (uint16_t)((ra << 11) | (ga << 5) | ba);
        s_sprite->fillSprite(col);
      }

    } else {
      // P5: fade to black from current color. Quadratic (v*v) so the
      // drop-off feels natural — fast at first, then eases into dark.
      float t = clamp01f((float)(e - p4End) / (float)(p5End - p4End));
      float v = 1.0f - t;
      uint16_t base = errorMode ? accent : C_WHITE;
      s_sprite->fillSprite(dim(base, v * v));
    }

    s_sprite->pushSprite(0, 0);
    paceFrame(frameMs, 33);
  }

  // Leave the screen clean black so whatever paints next (creature
  // `WAKING UP` text, wifi radar anim, etc.) starts from a known state.
  s_sprite->fillSprite(C_BG);
  s_sprite->pushSprite(0, 0);
  // DO NOT free the sprite — the wifi-join animation reuses it to
  // avoid a second 150 KB PSRAM allocation right after we finish.
}

// ---------------------------------------------------------------------------
// Wi-Fi radar animation state
// ---------------------------------------------------------------------------
struct Ring {
  uint32_t bornMs;
};
static constexpr int      WIFI_RING_MAX    = 6;
static constexpr uint32_t WIFI_RING_LIFE   = 1800;  // 0→~140px radius
static constexpr uint32_t WIFI_RING_SPAWN  = 450;   // ms between births
static constexpr uint32_t WIFI_TIMEOUT_MS  = 25000; // matches connectTo() cap
static constexpr int16_t  WIFI_RING_CX     = 120;
static constexpr int16_t  WIFI_RING_CY     = 158;
static constexpr int16_t  WIFI_RING_R_MIN  = 14;
static constexpr int16_t  WIFI_RING_R_MAX  = 138;

static Ring     s_rings[WIFI_RING_MAX];
static int      s_ringCount   = 0;
static uint32_t s_nextRingMs  = 0;
static String   s_wifiSsid;
static uint32_t s_wifiBeginMs = 0;

// Composite one frame of the radar into the shared sprite.
static void drawRadarFrame(uint32_t nowMs, uint32_t elapsedMs) {
  s_sprite->fillSprite(C_BG);

  // --- Title + SSID (static-ish header) ---------------------------------
  s_sprite->setTextDatum(TC_DATUM);
  s_sprite->setTextFont(2);
  s_sprite->setTextColor(C_ACCENT, C_BG);
  s_sprite->drawString("JOINING WIFI", CX, 28);

  // SSID: clamp so a 40-char hotspot name doesn't spill off-screen.
  String ssid = s_wifiSsid;
  if (ssid.length() == 0) ssid = "—";
  if (ssid.length() > 28) ssid = ssid.substring(0, 27) + "…";
  s_sprite->setTextColor(dim(C_ACCENT, 0.75f), C_BG);
  s_sprite->drawString(ssid, CX, 48);

  // --- Radar rings ------------------------------------------------------
  // Each ring has a lifetime (0..WIFI_RING_LIFE). Radius eases out so
  // the wave starts fast and slows as it reaches the edge — reads as
  // a proper radio ping. Alpha fades linearly over lifetime.
  for (int i = 0; i < s_ringCount; ++i) {
    uint32_t age = nowMs - s_rings[i].bornMs;
    if (age >= WIFI_RING_LIFE) continue;
    float t  = (float)age / (float)WIFI_RING_LIFE;
    int16_t r = WIFI_RING_R_MIN +
                (int16_t)((WIFI_RING_R_MAX - WIFI_RING_R_MIN) * easeOut(t));
    float  a = 1.0f - t;                  // fade over life
    uint16_t core = dim(C_ACCENT, a * 0.95f);
    uint16_t halo = dim(C_ACCENT, a * 0.30f);
    s_sprite->drawCircle(WIFI_RING_CX, WIFI_RING_CY, r,     core);
    if (r > 1) s_sprite->drawCircle(WIFI_RING_CX, WIFI_RING_CY, r - 1, halo);
    if (r < 160) s_sprite->drawCircle(WIFI_RING_CX, WIFI_RING_CY, r + 1, halo);
  }

  // --- Center "antenna" -------------------------------------------------
  // Bright pulsing white core at the origin of the rings, gently
  // breathing so the whole screen feels alive even when rings are mid-
  // expansion.
  float breath = 0.5f + 0.5f * sinf((float)nowMs * 0.004f);
  s_sprite->fillCircle(WIFI_RING_CX, WIFI_RING_CY, 6, dim(C_ACCENT, 0.35f));
  s_sprite->fillCircle(WIFI_RING_CX, WIFI_RING_CY, 3 + (int)(breath * 2.0f),
                       C_WHITE);

  // --- Animated "connecting…" dots --------------------------------------
  int dotCount = (int)(1 + (elapsedMs / 400) % 4);
  String dots  = "connecting";
  for (int i = 0; i < dotCount; ++i) dots += ".";
  s_sprite->setTextFont(2);
  s_sprite->setTextColor(dim(C_ACCENT, 0.85f), C_BG);
  s_sprite->drawString(dots, CX, 254);

  // After 18s, admit to the user that this might not work. Keeps the
  // anim from feeling frozen on a bad network.
  if (elapsedMs > 18000) {
    s_sprite->setTextColor(dim(C_AMBER, 0.85f), C_BG);
    s_sprite->drawString("still trying…", CX, 274);
  }

  // --- Progress bar -----------------------------------------------------
  const int16_t barW = 200;
  const int16_t barH = 3;
  const int16_t barX = (SCR_W - barW) / 2;
  const int16_t barY = 300;
  s_sprite->drawRect(barX, barY, barW, barH, dim(C_ACCENT, 0.35f));
  int16_t fill = (int16_t)(barW *
                 clamp01f((float)elapsedMs / (float)WIFI_TIMEOUT_MS));
  if (fill > 0) s_sprite->fillRect(barX, barY, fill, barH, C_ACCENT);

  s_sprite->pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Wi-Fi animation public API
// ---------------------------------------------------------------------------
void wifiJoinAnimBegin(TFT_eSPI *tft, const String &ssid) {
  if (!ensureSprite(tft)) return;
  s_wifiSsid    = ssid;
  s_wifiBeginMs = millis();
  s_ringCount   = 0;
  s_nextRingMs  = s_wifiBeginMs;   // first ring fires immediately
  drawRadarFrame(s_wifiBeginMs, 0);
}

void wifiJoinAnimTick(uint32_t elapsedMs) {
  if (!s_sprite) return;
  uint32_t now = millis();

  // Spawn rings on cadence. Ring lifetime (1800 ms) × spawn rate
  // (450 ms) = up to 4 simultaneous rings mid-expansion, which looks
  // pleasantly busy without visual clutter. The ring array is a tiny
  // ring buffer (pun intended) — when full we shift left.
  if (now >= s_nextRingMs) {
    if (s_ringCount < WIFI_RING_MAX) {
      s_rings[s_ringCount++].bornMs = now;
    } else {
      for (int i = 1; i < WIFI_RING_MAX; ++i) s_rings[i - 1] = s_rings[i];
      s_rings[WIFI_RING_MAX - 1].bornMs = now;
    }
    s_nextRingMs = now + WIFI_RING_SPAWN;
  }

  drawRadarFrame(now, elapsedMs);
}

void wifiJoinAnimEnd(bool success) {
  if (!s_sprite) return;
  uint32_t start = millis();
  if (success) {
    // Outro: single bright ring expands to fill the screen with a
    // "CONNECTED" label at center. Reads as an emphatic final ping.
    const uint32_t DUR = 320;
    uint32_t frameMs = millis();
    while (true) {
      uint32_t e = millis() - start;
      if (e >= DUR) break;
      float t = clamp01f((float)e / (float)DUR);
      float a = 1.0f - t;
      int16_t r = 14 + (int16_t)(260.0f * easeOut(t));

      s_sprite->fillSprite(C_BG);
      s_sprite->drawCircle(WIFI_RING_CX, WIFI_RING_CY, r,     dim(C_ACCENT, a));
      if (r > 1)
        s_sprite->drawCircle(WIFI_RING_CX, WIFI_RING_CY, r - 1, dim(C_ACCENT, a * 0.6f));
      if (r > 2)
        s_sprite->drawCircle(WIFI_RING_CX, WIFI_RING_CY, r - 2, dim(C_ACCENT, a * 0.25f));
      s_sprite->fillCircle(WIFI_RING_CX, WIFI_RING_CY, 4, C_WHITE);

      s_sprite->setTextDatum(TC_DATUM);
      s_sprite->setTextFont(2);
      s_sprite->setTextColor(dim(C_WHITE, 0.6f + 0.4f * (1.0f - t)), C_BG);
      s_sprite->drawString("CONNECTED", CX, 150);

      s_sprite->pushSprite(0, 0);
      paceFrame(frameMs, 33);
    }
  } else {
    // Failure outro: three red rings collapsing toward center with a
    // red "OFFLINE" label. Different shape from the success outro so
    // the outcome is legible at a glance without reading the text.
    const uint32_t DUR = 460;
    uint32_t frameMs = millis();
    while (true) {
      uint32_t e = millis() - start;
      if (e >= DUR) break;
      float t = clamp01f((float)e / (float)DUR);
      float a = 1.0f - t;

      s_sprite->fillSprite(C_BG);
      for (int i = 0; i < 3; ++i) {
        int16_t r = (int16_t)(WIFI_RING_R_MAX * (1.0f - easeInOut(t)))
                  - (int16_t)(i * 18);
        if (r <= 2) continue;
        uint16_t col = dim(C_RED, a * (i == 0 ? 0.95f : (i == 1 ? 0.55f : 0.25f)));
        s_sprite->drawCircle(WIFI_RING_CX, WIFI_RING_CY, r, col);
      }
      s_sprite->fillCircle(WIFI_RING_CX, WIFI_RING_CY, 3, dim(C_RED, a));

      s_sprite->setTextDatum(TC_DATUM);
      s_sprite->setTextFont(2);
      s_sprite->setTextColor(dim(C_RED, a * 1.0f + 0.1f), C_BG);
      s_sprite->drawString("OFFLINE", CX, 152);

      s_sprite->pushSprite(0, 0);
      paceFrame(frameMs, 33);
    }
  }

  // Final clean black frame + hand the PSRAM back. Whatever the main
  // code paints next (creature view) starts from a known clear state.
  s_sprite->fillSprite(C_BG);
  s_sprite->pushSprite(0, 0);
  freeSprite();
}
