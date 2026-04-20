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
  MOOD_ANGRY   = 5,   // sharper eyebrows, redder glow tint
  // Sleepy/low-activity states. Promoted to "first-class" moods so the
  // mood engine can paint them over idle when the human has been quiet
  // for a while. Visual treatment is driven centrally in creatureTick()
  // (droopy lids, slower bob + pulse), so all four face styles inherit
  // the effect without per-renderer changes.
  MOOD_BORED   = 6,   // lids at ~15% close, slower bob, eye drifts further
  MOOD_TIRED   = 7,   // lids at ~45% (half-mast), slower pulse, quieter mouth
  MOOD_SLEEP   = 8    // lids at ~85% (slits), tiny bob, mouth still
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
CreatureMood creatureMood();
void creatureSetTalking(bool on);

// Tell the creature to blink ASAP (used when it first wakes, etc).
void creatureForceBlink();

// Discrete one-shot overlays layered on top of the normal idle animation.
// Each reaction runs for a fixed duration (a few hundred ms to ~1.5 s) and
// temporarily overrides bob / gaze / blink / mouth to "sell" a punctuated
// moment — e.g. a wallet outflow should STARTLE Daemon, a long-silent
// human returning should make him PERK_UP, 3am-drowsy + no one around
// should DOZE once in a while. Safe to call at any rate; the renderer
// coalesces rapid triggers into the most recent one.
enum CreatureReaction : uint8_t {
  REACT_STARTLE  = 0,   // ~400 ms: quick upward bob + briefly wide eyes
  REACT_PERK_UP  = 1,   // ~600 ms: gaze recenters + happy-mood flash
  REACT_DROOP    = 2,   // ~800 ms: slow long blink + gaze sags down
  REACT_GRUMBLE  = 3,   // ~500 ms: eyes narrow + angry-tint flash
  REACT_DOZE     = 4,   // ~1400 ms: single extra-long sleepy blink
};
void creatureTriggerReaction(CreatureReaction r);

// Smoothly interpolate the creature's render scale toward `target`.
// 1.0  = normal / "close up".
// 0.55 = zoomed-out ("stepped back from the screen") preset used by
//        the long-press gesture in main.cpp.
// Values are clamped to [0.35, 1.0]. The interpolation happens inside
// creatureTick() at ~60 Hz with a 1st-order filter, so a single call
// per state change is enough — no need to animate by hand.
void creatureSetZoomTarget(float target);

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
