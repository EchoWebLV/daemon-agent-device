#include "mood.h"
#include "creature.h"
#include <time.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
// One process-wide instance. We don't bother with a mutex: every field is
// word-sized, writes happen either from the main loop (moodTick /
// moodNoteUserUtterance) or from the LLM worker via moodNoteDaemonReply,
// and readers (moodPromptContext / moodTemperature) only ever want a
// coherent-enough-looking snapshot. A torn read of a float would still be
// within range — we clamp on every update anyway. Keeping it lock-free
// matters because buildSystemPrompt() runs on the LLM task and we don't
// want to sit on a mutex there.
static DaemonState s = {};
static uint32_t    s_bootMs         = 0;
static uint32_t    s_lastUserMs     = 0;   // millis() of last user turn
static uint32_t    s_lastTickMs     = 0;   // rate-limit decay math to 1Hz

static inline float clamp01(float x)   { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static inline float clampMood(float x) { return x < -1 ? -1 : (x > 1 ? 1 : x); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void moodBegin() {
  s.mood        = 0.10f;   // very slightly positive out of the gate — fresh boot
  s.energy      = 0.85f;   // well-rested on cold boot
  s.curiosity   = 0.60f;   // a little curious about the new world
  s.snark       = 0.50f;   // baseline dry

  s.uptimeMs    = 0;
  s.silenceMs   = 0;
  s.hourLocal   = -1;
  s.isNight     = false;

  for (int i = 0; i < 3; ++i) s.recentReplies[i] = "";
  s.recentCount = 0;

  s_bootMs     = millis();
  s_lastUserMs = 0;        // 0 means "no user turn yet" — silenceMs stays 0
  s_lastTickMs = 0;
}

// ---------------------------------------------------------------------------
// Tick: cheap sampling + slow decay. Runs at most once per second.
// ---------------------------------------------------------------------------
void moodTick() {
  uint32_t now = millis();
  if (s_lastTickMs != 0 && (now - s_lastTickMs) < 1000) return;

  uint32_t dtMs = (s_lastTickMs == 0) ? 1000 : (now - s_lastTickMs);
  s_lastTickMs = now;
  float dtSec = (float)dtMs / 1000.0f;

  s.uptimeMs  = now - s_bootMs;
  s.silenceMs = (s_lastUserMs == 0) ? 0 : (now - s_lastUserMs);

  // Local clock, if NTP has synced. configTime() is called from setup()
  // once Wi-Fi is up — until then time() returns an epoch close to 0
  // and we just report hourLocal = -1 so the prompt can say "unknown".
  time_t t = time(nullptr);
  if (t > 1700000000) {   // ≈ any time after 2023-11 is "definitely NTP'd"
    struct tm lt;
    localtime_r(&t, &lt);
    s.hourLocal = (int8_t)lt.tm_hour;
    s.isNight   = (lt.tm_hour >= 22 || lt.tm_hour < 6);
  } else {
    s.hourLocal = -1;
    s.isNight   = false;
  }

  // --- energy -------------------------------------------------------------
  // Long-tail decay: full from boot, drifts down toward 0.2 over ~8 h of
  // uptime. Night hours pull another 0.15 off the steady-state floor so a
  // 3am Daemon always sounds at least a bit worn out.
  {
    const float hoursUp = (float)s.uptimeMs / 3600000.0f;
    float target = 0.85f * expf(-hoursUp / 5.5f) + 0.20f;   // 1.05→0.20
    if (s.isNight) target -= 0.15f;
    // 1st-order low-pass: energy walks toward target at ~1 unit / 15 min.
    float k = dtSec / (15.0f * 60.0f);
    s.energy += (target - s.energy) * clamp01(k);
    s.energy = clamp01(s.energy);
  }

  // --- curiosity ----------------------------------------------------------
  // Rises with silence (people-watching effect); plateaus around 0.9 after
  // ~40 min alone. Drops sharply whenever the user speaks (see
  // moodNoteUserUtterance). Night slightly suppresses it.
  {
    float silMin = (float)s.silenceMs / 60000.0f;
    float target = 1.0f - expf(-silMin / 20.0f);     // 0 → ~0.86 at 40min
    if (s.isNight) target *= 0.7f;
    float k = dtSec / (5.0f * 60.0f);
    s.curiosity += (target - s.curiosity) * clamp01(k);
    s.curiosity = clamp01(s.curiosity);
  }

  // --- mood ---------------------------------------------------------------
  // Baseline drift back toward 0 so old grumpiness doesn't stick forever.
  // Long silences past 2h nudge it mildly negative (feels abandoned),
  // sub-30-minute silences nudge it mildly positive (in-flow chat).
  {
    float silMin = (float)s.silenceMs / 60000.0f;
    float target = 0.0f;
    if (silMin > 120.0f)      target = -0.20f;
    else if (silMin < 15.0f && s_lastUserMs != 0) target = +0.15f;
    if (s.isNight && silMin > 60.0f) target -= 0.10f;
    float k = dtSec / (30.0f * 60.0f);   // very slow
    s.mood += (target - s.mood) * clamp01(k);
    s.mood = clampMood(s.mood);
  }

  // --- snark --------------------------------------------------------------
  // Drifts back toward 0.5 baseline. We'll let external hooks poke it
  // up/down later (interruptions, repeated questions, correction). For
  // now just keep it from running away.
  {
    float k = dtSec / (20.0f * 60.0f);
    s.snark += (0.50f - s.snark) * clamp01(k);
    s.snark = clamp01(s.snark);
  }

  // --- ambient reactions --------------------------------------------------
  // Punctuated moments that make the idle animation feel inhabited rather
  // than looped. Each is rate-limited via a separate next-allowed
  // timestamp so they can't pile up on top of each other.
  static uint32_t s_nextDozeMs      = 0;
  static uint32_t s_nextSadDroopMs  = 0;

  // Nightly doze: when it's late AND energy is very low AND the human
  // isn't around, trigger a single extra-long sleepy blink roughly every
  // 6-10 minutes. Gives Daemon the "dozing off" look that 3am on a
  // lived-in desk deserves.
  if (s.isNight && s.energy < 0.25f && s.silenceMs > 5UL * 60UL * 1000UL &&
      now >= s_nextDozeMs) {
    creatureTriggerReaction(REACT_DOZE);
    s_nextDozeMs = now + (uint32_t)(6UL * 60UL * 1000UL) +
                   (uint32_t)random(0, 4L * 60L * 1000L);
  }

  // Sad droop: when silence crosses 2h, fire once, then rearm only after
  // another hour so it doesn't nag. Reads as a slow dejected blink.
  if (s.silenceMs > 2UL * 60UL * 60UL * 1000UL && now >= s_nextSadDroopMs) {
    creatureTriggerReaction(REACT_DROOP);
    s_nextSadDroopMs = now + 60UL * 60UL * 1000UL;
  }

  // --- idle-class mood painting -------------------------------------------
  // Daemon's face cycles IDLE → BORED → TIRED → SLEEP as silence + low
  // energy pile up, and any of those promotions happens ONLY while the
  // creature is already in an idle-class mood. Active moods (LISTEN while
  // the user talks, THINK during LLM round-trips, TALK while Daemon is
  // speaking, HAPPY / ANGRY during reaction overlays) are left alone so
  // we never yank the face out of a foreground state mid-interaction.
  //
  // We measure silence from boot when the user hasn't spoken yet: without
  // this, a device that's never been chatted with stays permanently IDLE
  // because silenceMs is pinned at 0. Using uptime as the fallback gives
  // a freshly-flashed board the correct "he'll get bored if left alone"
  // behavior.
  {
    CreatureMood cur = creatureMood();
    bool isIdleClass = (cur == MOOD_IDLE  || cur == MOOD_BORED ||
                        cur == MOOD_TIRED || cur == MOOD_SLEEP);
    if (isIdleClass) {
      uint32_t effSilence = (s_lastUserMs == 0) ? s.uptimeMs : s.silenceMs;
      float silMin = (float)effSilence / 60000.0f;

      // Deepest state first — the conditions below are INTENTIONALLY
      // cumulative. Night rules kick in earlier so late-evening Daemon
      // visibly winds down without waiting 30 minutes.
      CreatureMood want = MOOD_IDLE;
      if ((s.energy < 0.22f && silMin > 30.0f) ||
          (s.isNight && silMin > 10.0f && s.energy < 0.35f)) {
        want = MOOD_SLEEP;
      } else if (s.energy < 0.38f || silMin > 15.0f) {
        want = MOOD_TIRED;
      } else if (silMin > 2.5f) {
        want = MOOD_BORED;
      }
      if (want != cur) creatureSetMood(want);
    }
  }
}

// ---------------------------------------------------------------------------
// External signals
// ---------------------------------------------------------------------------
void moodNoteUserUtterance() {
  uint32_t now  = millis();
  uint32_t prev = s_lastUserMs;
  s_lastUserMs  = now;

  // If Daemon had drifted into a sleepy idle state, snap him back to
  // plain IDLE immediately. The PERK_UP reaction below (for long-gap
  // returns) still plays on top; for short-gap returns we at least
  // reopen the eyelids so his first "reply" frame doesn't look like
  // he talked with his eyes closed.
  {
    CreatureMood cur = creatureMood();
    if (cur == MOOD_BORED || cur == MOOD_TIRED || cur == MOOD_SLEEP) {
      creatureSetMood(MOOD_IDLE);
    }
  }

  // User just spoke — curiosity spent, a small energy bump from engagement,
  // mood nudges slightly positive.
  s.curiosity = clamp01(s.curiosity * 0.55f);
  s.energy    = clamp01(s.energy    + 0.05f);
  s.mood      = clampMood(s.mood    + 0.05f);

  // "Asking again within 30s" — they're probably frustrated. Bump snark
  // up a hair so Daemon gets the hint.
  if (prev != 0 && (now - prev) < 30000) {
    s.snark = clamp01(s.snark + 0.10f);
  }

  // Visible reaction tied to the gap since last utterance:
  //   - No prior / very long silence (>20 min) → perk-up ("oh, you're back").
  //   - Rapid repeat (<30 s) → faint grumble to sell the snark bump above.
  //   - Everything else → nothing, so ordinary back-and-forth doesn't flash.
  if (prev == 0 || (now - prev) > 20UL * 60UL * 1000UL) {
    creatureTriggerReaction(REACT_PERK_UP);
  } else if ((now - prev) < 30000) {
    creatureTriggerReaction(REACT_GRUMBLE);
  }
}

void moodNoteActivity() {
  // Touch / swipe / long-press / any on-device "the human is fiddling
  // with the board" signal. Snaps sleepy idle states back to plain IDLE
  // with a small PERK_UP flash and a mild energy bump, but deliberately
  // does NOT touch s_lastUserMs — a poke isn't a conversation, and we
  // don't want one to hide the "alone with your thoughts" signal from
  // the LLM persona block on the next real reply.
  CreatureMood cur = creatureMood();
  if (cur == MOOD_BORED || cur == MOOD_TIRED || cur == MOOD_SLEEP) {
    creatureSetMood(MOOD_IDLE);
    s.energy = clamp01(s.energy + 0.10f);
    creatureTriggerReaction(REACT_PERK_UP);
  }
}

void moodNoteDaemonReply(const String &text) {
  // Slide ring left; newest goes to index (recentCount-1) once saturated.
  if (s.recentCount < 3) {
    s.recentReplies[s.recentCount] = text;
    s.recentCount++;
  } else {
    s.recentReplies[0] = s.recentReplies[1];
    s.recentReplies[1] = s.recentReplies[2];
    s.recentReplies[2] = text;
  }
  // Talking spends curiosity.
  s.curiosity = clamp01(s.curiosity * 0.75f);
}

const DaemonState &moodState() { return s; }

// ---------------------------------------------------------------------------
// Prompt fragment
// ---------------------------------------------------------------------------
static const char *energyWord(float e) {
  if (e < 0.25f) return "worn out";
  if (e < 0.45f) return "a bit tired";
  if (e < 0.70f) return "steady";
  return "wired and alert";
}
static const char *moodWord(float m) {
  if (m < -0.45f) return "grumpy";
  if (m < -0.15f) return "a little off";
  if (m <  0.15f) return "neutral";
  if (m <  0.45f) return "good-humored";
  return "genuinely up";
}
static const char *curiosityWord(float c) {
  if (c < 0.25f) return "not much is catching your eye";
  if (c < 0.55f) return "mildly curious";
  if (c < 0.80f) return "itching to notice something";
  return "very curious — you've been alone with your thoughts";
}
static String uptimeWord(uint32_t ms) {
  uint32_t sec = ms / 1000;
  uint32_t h   = sec / 3600;
  uint32_t m   = (sec % 3600) / 60;
  char buf[32];
  if (h == 0)      snprintf(buf, sizeof(buf), "%um", (unsigned)m);
  else             snprintf(buf, sizeof(buf), "%uh %02um", (unsigned)h, (unsigned)m);
  return String(buf);
}
static String silenceWord(uint32_t ms, bool userHasSpoken) {
  if (!userHasSpoken) return String("your human hasn't said anything since you booted");
  uint32_t sec = ms / 1000;
  if (sec < 60)   return String("they just spoke");
  uint32_t m = sec / 60;
  if (m < 60) {
    char buf[64]; snprintf(buf, sizeof(buf), "%um since they last spoke", (unsigned)m);
    return String(buf);
  }
  uint32_t h = m / 60; uint32_t mm = m % 60;
  char buf[64]; snprintf(buf, sizeof(buf), "%uh %02um since they last spoke", (unsigned)h, (unsigned)mm);
  return String(buf);
}

String moodPromptContext() {
  // Snapshot everything under a single read so the rendered block is
  // internally consistent even if a tick fires mid-build.
  DaemonState c = s;

  String out;
  out.reserve(512);
  out += "\n---\nINTERNAL STATE (right now — let this color how you answer, don't recite it):\n";

  // Time / uptime line.
  out += "- ";
  if (c.hourLocal >= 0) {
    char t[32];
    snprintf(t, sizeof(t), "local time ~%02d:00%s, ",
             (int)c.hourLocal,
             c.isNight ? " (late — voices are quieter at night)" : "");
    out += t;
  } else {
    out += "time unknown (NTP not synced yet), ";
  }
  out += "you've been awake ";
  out += uptimeWord(c.uptimeMs);
  out += ".\n";

  // Mood / energy / curiosity line.
  out += "- you feel ";
  out += moodWord(c.mood);
  out += " and ";
  out += energyWord(c.energy);
  out += "; ";
  out += curiosityWord(c.curiosity);
  out += ".\n";

  // Silence line.
  out += "- ";
  out += silenceWord(c.silenceMs, s_lastUserMs != 0);
  out += ".\n";

  // Snark knob — phrased as a rule, not a number.
  if (c.snark > 0.65f) {
    out += "- you're in a dry-sarcastic mood; let one understated jab land if it fits, then move on.\n";
  } else if (c.snark < 0.30f) {
    out += "- keep it sincere this turn — no jokes.\n";
  }

  // Anti-repeat block: show the last ~3 replies and forbid reusing phrases.
  // Trim each to 140 chars so the prompt doesn't balloon.
  if (c.recentCount > 0) {
    out += "\nRECENT THINGS YOU ALREADY SAID (do NOT reuse these sentence openers, "
           "signature phrases, or punchlines — find a different angle, a different rhythm, "
           "or a different first word):\n";
    for (uint8_t i = 0; i < c.recentCount; ++i) {
      String r = c.recentReplies[i];
      r.replace("\n", " ");
      r.trim();
      if (r.length() > 140) r = r.substring(0, 137) + "…";
      out += "  • \"";
      out += r;
      out += "\"\n";
    }
  }

  // Gentle variance nudge — tells the model to actually USE the state
  // above instead of defaulting to its favorite opener.
  out += "\nIf your first instinct is a phrase like \"Looks like\", \"Well,\", "
         "\"Ah,\", or any opener you've used recently, pick something else. "
         "Vary sentence length too — sometimes one clipped line is enough.\n";

  return out;
}

// ---------------------------------------------------------------------------
// Generation tuning derived from state
// ---------------------------------------------------------------------------
float moodTemperature() {
  // Base 0.65; + up to 0.30 for curiosity, + up to 0.10 for snark, − up
  // to 0.10 for very low energy (tired = more predictable).
  float t = 0.65f
          + 0.30f * s.curiosity
          + 0.10f * s.snark
          - 0.10f * (1.0f - s.energy) * (s.energy < 0.3f ? 1.0f : 0.0f);
  if (t < 0.55f) t = 0.55f;
  if (t > 1.05f) t = 1.05f;
  return t;
}

int moodMaxTokens() {
  // Tired Daemon → terser replies; wired + curious → room to stretch.
  if (s.energy < 0.25f) return 140;
  if (s.energy < 0.50f) return 260;
  if (s.curiosity > 0.80f) return 512;
  return 380;
}
