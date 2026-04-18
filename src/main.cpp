// ============================================================================
//  Blue Gremlin — an animated creature that lives on the Waveshare
//  ESP32-S3-Touch-LCD-2.8 and talks to you through Gemini.
//
//  Hardware used:
//    - ST7789 240x320 LCD (TFT_eSPI pins in platformio.ini)
//    - PCM5101 I2S speaker  (BCK=48, LRCK=38, DIN=47)
//
//  Flow:
//    1. Boot → draw the creature → connect to Wi-Fi
//    2. You visit http://<board-ip>/ on your phone, tap the big mic, talk.
//    3. Your phone transcribes you with the Web Speech API and POSTs the
//       text to /say.
//    4. The board forwards your text to Gemini (with a personality prompt),
//       pipes the reply through StreamElements TTS, and streams the MP3 out
//       the speaker. The creature's mouth animates for the whole duration.
//
//  Dev aid: you can also type a message over the USB serial monitor and the
//  gremlin will reply the same way, which is handy when Wi-Fi STT isn't
//  available.
// ============================================================================
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <TFT_eSPI.h>

#include "creature.h"
#include "voice.h"
#include "ai.h"
#include "server.h"
#include "secrets.h"

static TFT_eSPI tft;
static bool     s_wifiOk   = false;
static bool     s_voiceOk  = false;

// Ambient Solana chatter: Daemon spontaneously drops a fun fact every so
// often when he's otherwise idle. Tuned to roughly once per minute, with a
// bit of jitter so it doesn't feel mechanical.
static constexpr uint32_t SOLANA_MIN_INTERVAL_MS = 60000;   // 60 s
static constexpr uint32_t SOLANA_JITTER_MS       = 15000;   // +0..15 s
static uint32_t s_nextSolanaMs = 0;

static void scheduleNextSolana() {
  s_nextSolanaMs = millis() + SOLANA_MIN_INTERVAL_MS
                            + (uint32_t)random(0, SOLANA_JITTER_MS);
}

// Prompts that nudge Gemini to produce a short, surprising Solana tidbit.
// We cycle through a few so the replies don't drift toward the same facts.
static const char *SOLANA_PROMPTS[] = {
  "Tell me ONE surprising, specific fact about Solana the blockchain. "
  "One sentence, under 220 characters, spoken-word friendly. No lists, "
  "no disclaimers. Stay in character as Daemon.",

  "Share a weird or little-known thing about how Solana's technology works "
  "(Proof of History, Gulf Stream, Sealevel, Firedancer, etc). "
  "One sentence, under 220 characters, stay in character as Daemon.",

  "Drop a fun piece of Solana trivia — could be a milestone, a record, a "
  "famous outage, an ecosystem project, an NFT collection, whatever is "
  "most interesting to you right now. One sentence, under 220 characters, "
  "stay in character as Daemon.",

  "Give me your hot take on something in the Solana ecosystem — a meme "
  "coin, a DePIN project, mobile Saga, Jupiter, whatever. One sentence, "
  "under 220 characters, stay in character as Daemon.",
};
static constexpr int N_SOLANA_PROMPTS =
    sizeof(SOLANA_PROMPTS) / sizeof(SOLANA_PROMPTS[0]);

// ---------------------------------------------------------------------------
// The core "ask Gemini + speak" pipeline.
// ---------------------------------------------------------------------------
static void handleUtterance(const String &user) {
  Serial.print(">> you:    "); Serial.println(user);

  creatureSetMood(MOOD_THINK);
  creatureSetSubtitle("you:", user);
  serverSetStatus("thinking…");

  uint32_t t0 = millis();
  String reply;
  bool ok = aiAsk(user, reply);
  Serial.printf("ai: aiAsk took %lu ms (ok=%d)\n",
                (unsigned long)(millis() - t0), ok ? 1 : 0);
  if (!ok || reply.length() == 0) {
    reply = "I got nothing. Ask me again?";
  }
  Serial.print("<< daemon: "); Serial.println(reply);
  serverSetReply(user, reply);
  creatureSetSubtitle("daemon:", reply);

  if (s_voiceOk) {
    creatureSetMood(MOOD_TALK);
    creatureSetTalking(true);
    serverSetStatus("speaking…");
    voiceSpeak(reply);
  } else {
    creatureSetMood(MOOD_IDLE);
    serverSetStatus("idle");
  }

  // Any time he talks (for whatever reason), delay the next ambient Solana
  // fact so he doesn't immediately start rambling again.
  scheduleNextSolana();
}

// ---------------------------------------------------------------------------
// Spontaneous chatter: Daemon shares something interesting about Solana
// every ~60-75 seconds while he's idle.
// ---------------------------------------------------------------------------
static void maybeSpeakSolanaFact() {
  if (!s_wifiOk || !s_voiceOk) return;
  if (voiceIsSpeaking())       return;
  if (millis() < s_nextSolanaMs) return;

  // Heartbeat so we can actually see the timer firing in the serial log.
  static uint32_t s_lastHeartbeatMs = 0;
  uint32_t now = millis();
  if (now - s_lastHeartbeatMs > 5000) {
    Serial.printf("solana: firing (elapsed since schedule=%ld ms)\n",
                  (long)(now - s_nextSolanaMs));
    s_lastHeartbeatMs = now;
  }

  const char *prompt = SOLANA_PROMPTS[random(0, N_SOLANA_PROMPTS)];
  Serial.print(".. solana prompt: "); Serial.println(prompt);

  creatureSetMood(MOOD_THINK);
  creatureSetSubtitle("solana:", "thinking…");
  serverSetStatus("solana…");

  uint32_t t0 = millis();
  String reply;
  bool ok = aiAskOneShot(prompt, reply);
  Serial.printf("ai: solana oneshot took %lu ms (ok=%d)\n",
                (unsigned long)(millis() - t0), ok ? 1 : 0);

  if (!ok || reply.length() == 0) {
    Serial.println("solana: skipped (ai failed)");
    creatureSetSubtitle("daemon:", "(couldn't reach Gemini)");
    scheduleNextSolana();
    creatureSetMood(MOOD_IDLE);
    serverSetStatus("idle");
    return;
  }
  Serial.print("<< daemon (solana): "); Serial.println(reply);
  serverSetReply("(solana fact)", reply);
  creatureSetSubtitle("daemon:", reply);

  creatureSetMood(MOOD_TALK);
  creatureSetTalking(true);
  serverSetStatus("speaking…");
  voiceSpeak(reply);
  scheduleNextSolana();
}

// Periodic status ticker so we can tell from the serial log WHY the Solana
// timer isn't firing (e.g., "still speaking", "42 s until next fact"). The
// log line prints at most once every 10 s.
static void logSolanaTickerState() {
  if (!s_wifiOk || !s_voiceOk) return;
  static uint32_t s_lastLogMs = 0;
  uint32_t now = millis();
  if (now - s_lastLogMs < 10000) return;
  s_lastLogMs = now;

  if (voiceIsSpeaking()) {
    Serial.println("solana: paused — daemon is speaking");
    return;
  }
  long diff = (long)s_nextSolanaMs - (long)now;
  if (diff > 0) {
    Serial.printf("solana: next fact in ~%ld s\n", diff / 1000);
  }
}

// ---------------------------------------------------------------------------
// Serial input reader (dev fallback). Press "Enter" on a non-empty line
// in the monitor to send the line as if you'd spoken it.
// ---------------------------------------------------------------------------
static String s_serialBuf;
static void pumpSerialInput() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      s_serialBuf.trim();
      if (s_serialBuf.length() > 0) handleUtterance(s_serialBuf);
      s_serialBuf = "";
    } else {
      s_serialBuf += (char)c;
      if (s_serialBuf.length() > 500) s_serialBuf = "";
    }
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(80);
  Serial.println();
  Serial.println("== Daemon booting ==");

  pinMode(5, OUTPUT);      // LCD backlight
  digitalWrite(5, HIGH);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(0x0000);

  if (!creatureBegin(&tft)) {
    Serial.println("creature: begin failed");
    tft.setTextColor(0xF800);
    tft.setCursor(10, 20);
    tft.print("creature sprite alloc failed");
  }
  creatureSetStatus("WAKING UP");
  creatureTick();   // first frame

  s_voiceOk = voiceBegin();
  Serial.printf("voice: %s\n", s_voiceOk ? "OK" : "failed");

  // Wi-Fi + web server. These are best-effort; creature still animates
  // without them. That lets you iterate on the drawing without a network.
  creatureSetStatus("JOINING WIFI");
  creatureTick();
  s_wifiOk = serverBeginWifi();
  if (s_wifiOk) {
    aiBegin();
    serverBeginHttp(handleUtterance);
    String ipLine = "http://" + serverLocalIP();
    creatureSetStatus(ipLine);
    serverSetStatus("idle");
    Serial.print("open "); Serial.print(ipLine); Serial.println(" on your phone");
  } else {
    creatureSetStatus("OFFLINE - TYPE IN SERIAL");
  }

  creatureForceBlink();
  creatureSetMood(MOOD_IDLE);

  // Boot-time voice test so you can hear immediately whether the speaker
  // is wired up. If you hear nothing, the 8Ω JST speaker isn't plugged in.
  if (s_wifiOk && s_voiceOk) {
    creatureSetMood(MOOD_TALK);
    creatureSetTalking(true);
    creatureSetSubtitle("daemon:", "Daemon online.");
    voiceSpeak("Daemon online. Talk to me through the web page.");
  }

  // First Solana drop comes ~45 s after boot so Daemon's hello message has
  // time to finish first.
  s_nextSolanaMs = millis() + 45000;
}

void loop() {
  // HTTP is blocking-per-request, but handleClient() returns quickly if no
  // client is connected, so the animation keeps running.
  if (s_wifiOk) serverLoop();

  // Drive the creature. We track talking-edge so we can drop the mouth
  // animation and restore the status line when the reply finishes playing.
  static bool wasTalking = false;
  bool talking = voiceIsSpeaking();
  creatureSetTalking(talking);
  if (wasTalking && !talking) {
    creatureSetMood(MOOD_IDLE);
    serverSetStatus("idle");
  }
  wasTalking = talking;
  creatureTick();

  voiceLoop();
  pumpSerialInput();
  maybeSpeakSolanaFact();
  logSolanaTickerState();

  // Target ~30 FPS for animation.
  delay(16);
}
