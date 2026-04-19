#include "transition.h"

static constexpr int16_t SCR_W      = 240;
static constexpr int16_t SCR_H      = 320;

// 14 frames at ~15 ms inherent SPI push time per frame (one 240×320
// 16-bpp sprite = ~10 ms over the 80-MHz bus, plus loop overhead) lands
// the transition at ~210 ms — snappy without feeling rushed. Tunable.
static constexpr int      FRAMES    = 14;

// PSRAM sprites holding the pre-rendered outgoing + incoming screens.
// Allocated once at boot; kept around for the life of the program.
// 2 × 240 × 320 × 2 bytes = 307 200 bytes — trivial in 8 MB PSRAM.
static TFT_eSPI    *s_tft   = nullptr;
static TFT_eSprite *s_inBuf  = nullptr;
static TFT_eSprite *s_outBuf = nullptr;

bool transitionBegin(TFT_eSPI *tft) {
  s_tft = tft;
  if (!tft) return false;

  s_outBuf = new TFT_eSprite(tft);
  s_outBuf->setColorDepth(16);
  // PSRAM_ENABLE = 3 in TFT_eSPI (see TFT_eSPI.h `setAttribute()` docs).
  // With it set, createSprite() allocates the buffer in PSRAM instead of
  // internal DRAM, which is essential here — internal DRAM is mostly
  // committed to the creature face sprite + task stacks + TLS buffers.
  s_outBuf->setAttribute(3 /*PSRAM_ENABLE*/, 1);
  if (!s_outBuf->createSprite(SCR_W, SCR_H)) {
    Serial.println("transition: outgoing sprite alloc FAILED — animations disabled");
    delete s_outBuf; s_outBuf = nullptr;
    return false;
  }
  // PSRAM-backed sprites need explicit byte-swap-on-push so the RGB565
  // pixels arrive on the SPI bus in the order ST7789 expects. Without
  // this, neon-blue 0x0AFF gets pushed as 0xFF0A and shows yellow.
  s_outBuf->setSwapBytes(true);

  s_inBuf = new TFT_eSprite(tft);
  s_inBuf->setColorDepth(16);
  s_inBuf->setAttribute(3 /*PSRAM_ENABLE*/, 1);
  if (!s_inBuf->createSprite(SCR_W, SCR_H)) {
    Serial.println("transition: incoming sprite alloc FAILED — animations disabled");
    s_outBuf->deleteSprite();
    delete s_outBuf; s_outBuf = nullptr;
    delete s_inBuf;  s_inBuf  = nullptr;
    return false;
  }
  s_inBuf->setSwapBytes(true);

  Serial.println("transition: PSRAM sprites ready (2 × 240×320 × 16bpp)");
  return true;
}

// Ease-out quadratic — fast at the start, slows into the resting position.
// Feels natural for "card slides in" UX (matches iOS push animation curve).
static inline float easeOut(float t) {
  float u = 1.0f - t;
  return 1.0f - u * u;
}

void transitionRunSlide(SlideDir dir,
                        TransitionDraw drawIncoming,
                        TransitionDraw drawOutgoing) {
  if (!s_tft || !s_inBuf || !s_outBuf) {
    // No sprites available — degrade to instant snap.
    if (drawIncoming) {
      // We can't pass the real TFT to a TransitionDraw that expects a
      // TFT_eSprite*, so the fallback path is "do nothing" — the
      // normal switchScreen() final draw call will paint the screen.
    }
    return;
  }

  // Pre-render both screens into their sprites. If a side is nullptr,
  // leave that sprite as whatever was last drawn (cheap "snap" fallback).
  if (drawOutgoing) {
    s_outBuf->fillSprite(0x0000);
    drawOutgoing(s_outBuf);
  }
  if (drawIncoming) {
    s_inBuf->fillSprite(0x0000);
    drawIncoming(s_inBuf);
  }

  // Animate. Each frame computes a motion offset p in [0..SCR_H] and
  // pushes complementary slices of the two sprites onto the live TFT.
  //
  // Push-order matters for tear-free output: we always push the
  // *receding* sprite first (the one whose visible area is shrinking)
  // and the *advancing* sprite second. That way, the strip of pixels
  // that's transitioning ownership this frame gets its FINAL value
  // last, so the user never sees the ~5 ms intermediate state where
  // it still holds the previous frame's contents.
  //
  // No explicit delay() — pushSprite over the 80 MHz SPI bus already
  // paces us at ~15 ms/frame, landing total animation around ~210 ms
  // for FRAMES=14.
  for (int i = 1; i <= FRAMES; ++i) {
    float t = (float)i / (float)FRAMES;
    int16_t p = (int16_t)(easeOut(t) * (float)SCR_H);
    if (p < 0)     p = 0;
    if (p > SCR_H) p = SCR_H;

    if (dir == SLIDE_UP) {
      // Outgoing slides up off the top (receding from the bottom),
      // incoming rises from the bottom (advancing into the bottom).
      // Push outgoing FIRST so the bottom strip ends as incoming.
      if (p < SCR_H) {
        s_outBuf->pushSprite(0, 0, 0, p, SCR_W, SCR_H - p);
      }
      if (p > 0) {
        s_inBuf->pushSprite(0, SCR_H - p, 0, 0, SCR_W, p);
      }
    } else {
      // SLIDE_DOWN: outgoing slides down off the bottom (receding from
      // the top), incoming drops in from the top.
      if (p < SCR_H) {
        s_outBuf->pushSprite(0, p, 0, 0, SCR_W, SCR_H - p);
      }
      if (p > 0) {
        s_inBuf->pushSprite(0, 0, 0, SCR_H - p, SCR_W, p);
      }
    }
  }

  // NOTE: the last loop iteration at i=FRAMES has p=SCR_H, which
  // pushes the entire incoming sprite to (0, 0). No follow-up
  // pushSprite needed — doing one anyway introduced a subtle flash.
  // The caller is also responsible for skipping its usual final
  // *Draw() call so we don't trigger a second fillScreen+repaint.
}
