// ============================================================================
//  Daemon mood / inner-state tracker.
//
//  The AI used to feel interchangeable because every prompt was identical:
//  a static persona + a wallet dump. This module maintains a small bundle
//  of slow-moving scalars (mood, energy, curiosity, snark) plus cheap
//  external context (uptime, local hour, silence-since-last-user-turn)
//  and assembles it into a short natural-language block that gets
//  appended to the system prompt. Two effects:
//
//    1. The LLM sees a DIFFERENT prompt each turn, even when the user
//       asks the same thing at 3am vs noon, or after 10 minutes of
//       silence vs mid-conversation — replies stop collapsing into the
//       same 3 vibes.
//    2. Temperature / token budget are scaled by mood, so a tired
//       Daemon is terse and dry and a wired Daemon is chattier and
//       more associative. Small change, huge perceived-aliveness win.
//
//  Nothing in here calls the LLM or touches the network. Updates are
//  o(1), safe to call from loop() at 60 Hz, and from the LLM worker
//  task when a reply lands. moodTick() internally rate-limits the
//  heavier decay math to once per second.
// ============================================================================
#pragma once
#include <Arduino.h>

// Slow-moving internal weather. All four are clamped to their ranges.
struct DaemonState {
  float mood;        // -1 (grumpy)  .. +1 (chipper)    — drifts toward 0
  float energy;      //  0 (drowsy)  .. +1 (wired)      — decays with uptime
  float curiosity;   //  0 (bored)   .. +1 (poking)     — rises with silence
  float snark;       //  0 (deadpan) .. +1 (dry sarcastic) — baseline ~0.5

  // Derived context sampled at tick time.
  uint32_t uptimeMs;
  uint32_t silenceMs;       // since last user utterance
  int8_t   hourLocal;       // 0..23 if NTP synced, else -1
  bool     isNight;         // 22:00 .. 06:00

  // Rolling anti-repeat ring: the last 3 assistant replies, newest last.
  String   recentReplies[3];
  uint8_t  recentCount;     // 0..3
};

// Call once in setup() after Serial is up. Seeds the state and records
// boot time. Cheap: no I/O.
void moodBegin();

// Call every loop iteration. Internally rate-limited; you can invoke
// it every frame without worry.
void moodTick();

// Signal points — call these from the existing call sites.
void moodNoteUserUtterance();                 // fires right before handleUtterance
void moodNoteDaemonReply(const String &text); // fires when a reply lands in drainLlmReplies

// Read-only accessors. Safe to call from any task.
const DaemonState &moodState();

// Render the block that gets appended to the system prompt. Returns a
// compact, natural-language description of how Daemon "feels" right
// now plus a short list of phrases to avoid reusing verbatim (the last
// 3 replies). Designed to be concatenated after the persona + wallet
// state; total length is typically 250-450 chars.
String moodPromptContext();

// LLM-call tuning derived from current state. Hotter / longer when
// Daemon is wired + curious; cooler / terser when drowsy + grumpy.
float moodTemperature();   // ~0.55 .. 1.05
int   moodMaxTokens();     // 140  .. 512
