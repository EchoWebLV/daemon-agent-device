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

// Advance animation by one frame. Call as often as you like (~20 Hz is
// plenty); internal timing handles smoothing.
void creatureTick();

// Mood + talk state. Both are independent: e.g. MOOD_LISTEN + talking=false
// while the user speaks; MOOD_TALK + talking=true while the creature replies.
void creatureSetMood(CreatureMood m);
void creatureSetTalking(bool on);

// Tell the creature to blink ASAP (used when it first wakes, etc).
void creatureForceBlink();

// Draw a small status string in the top status bar (Wi-Fi IP, etc).
void creatureSetStatus(const String &s);

// Subtitle area under Daemon. Pass an empty string to clear it. Word-wraps
// across up to 3 lines; anything longer is truncated with an ellipsis.
// The prefix (e.g. "daemon:" or "you:") is rendered in a distinct color.
void creatureSetSubtitle(const String &prefix, const String &text);
