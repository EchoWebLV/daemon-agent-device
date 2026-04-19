// ============================================================================
//  Blue Gremlin — procedural creature renderer for the 240x320 ST7789.
//
//  We draw the body silhouette (cloud-shape with two pointed ears, bright
//  blue glow outline, black interior) to the screen once at boot, then
//  animate only the face (eyes + mouth) every frame. A small PSRAM sprite
//  caches the body pixels inside the animated area so we can "erase" cleanly
//  without recomputing the whole silhouette.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

enum CreatureMood : uint8_t {
  MOOD_IDLE    = 0,   // slow eye-glow pulse, rare blinks
  MOOD_LISTEN  = 1,   // brighter steady glow, eyes slightly wider
  MOOD_THINK   = 2,   // eyes look up-and-right, slow pulse
  MOOD_TALK    = 3,   // mouth animates open/close
  MOOD_HAPPY   = 4,   // eyes squint upward in a smile arc
  MOOD_ANGRY   = 5    // sharper eyebrows, redder glow tint
};

// Initialise internal sprites and draw the static body to `tft`. Must be
// called after tft.init(). Returns false if sprite allocation fails.
bool creatureBegin(TFT_eSPI *tft);

// Full repaint — clears the whole screen and redraws the body, status bar,
// and subtitle. Call after coming back from a different screen (e.g. the
// wallet view) so Daemon reappears cleanly.
void creatureRepaint();

// Re-render the creature view (background + status + subtitle + last
// face frame) into an off-screen sprite. Used by the slide-transition
// module to pre-render Daemon into the moving sprite without disturbing
// the live face buffer's animation state.
void creatureDrawTo(TFT_eSprite *target);

// Advance animation by one frame. Call as often as you like (~20 Hz is
// plenty); internal timing handles smoothing.
void creatureTick();

// Mood + talk state. Both are independent: e.g. MOOD_LISTEN + talking=false
// while the user speaks; MOOD_TALK + talking=true while the creature replies.
void creatureSetMood(CreatureMood m);
void creatureSetTalking(bool on);

// Tell the creature to blink ASAP (used when it first wakes, etc).
void creatureForceBlink();

// Draw a small status string on the LEFT of the top status bar
// (Wi-Fi IP, "thinking", etc).
void creatureSetStatus(const String &s);

// Draw a small ticker on the RIGHT of the top status bar
// (e.g. "SOL $204.37"). Pass empty string to hide.
void creatureSetPrice(const String &s);

// Subtitle area under Daemon. Pass an empty string to clear it. Word-wraps
// across up to 3 lines; anything longer is truncated with an ellipsis.
// Every line is center-aligned.
void creatureSetSubtitle(const String &text);

// Face style selector.
//   0 = Daemon     (default neon almond eyes)
//   1 = Robot      (round glowing eyes with a pupil)
//   2 = Toy Robot  (blue chassis + red antenna + grille mouth)
//   3 = Calculator (= | 7-segment LCD look)
// Values outside 0..3 are clamped. Triggers a full face repaint.
void creatureSetFaceStyle(uint8_t style);
uint8_t creatureFaceStyle();
