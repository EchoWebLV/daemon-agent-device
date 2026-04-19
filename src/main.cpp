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
#include "wallet.h"
#include "price.h"
#include "memory.h"
#include "arweave.h"
#include "touch.h"
#include "walletscreen.h"
#include "settingsscreen.h"
#include "wifiscreen.h"
#include "devcfg.h"
#include "secrets.h"

static TFT_eSPI tft;
static bool     s_wifiOk   = false;
static bool     s_voiceOk  = false;

// Wallet + price refresh cadence.
static constexpr uint32_t PRICE_INTERVAL_MS  = 30000;   // 30 s
static constexpr uint32_t WALLET_INTERVAL_MS = 60000;   // 60 s
static uint32_t s_nextPriceMs  = 0;
static uint32_t s_nextWalletMs = 0;

// Heartbeat state. Tracks when the last scheduled prompt fired so we can
// space repeat runs by `devcfgHeartbeatIntervalMin()` regardless of what
// the user reconfigures in the middle of a cycle.
static uint32_t s_lastHeartbeatMs = 0;
static bool     s_heartbeatWasOn  = false;

// ---------------------------------------------------------------------------
// LLM worker task. Processes chat requests off the Arduino loop so the
// creature animation, touch, and HTTP polling stay live during the 10-30 s
// round-trip (LLM + tool x402 payments + follow-up LLM rounds).
// ---------------------------------------------------------------------------
struct LlmReq { char text[1024]; };
struct LlmReply {
  char user[1024];
  char reply[1600];
  bool ok;
};
static QueueHandle_t s_llmIn  = nullptr;
static QueueHandle_t s_llmOut = nullptr;
static volatile bool s_llmBusy = false;

static void llmWorkerTask(void *) {
  for (;;) {
    LlmReq req;
    if (xQueueReceive(s_llmIn, &req, portMAX_DELAY) != pdTRUE) continue;
    s_llmBusy = true;

    String user = String(req.text);
    String reply;
    bool ok = aiAsk(user, reply);

    LlmReply out;
    memset(&out, 0, sizeof(out));
    size_t un = user.length();
    if (un >= sizeof(out.user)) un = sizeof(out.user) - 1;
    memcpy(out.user, user.c_str(), un);
    size_t rn = reply.length();
    if (rn >= sizeof(out.reply)) rn = sizeof(out.reply) - 1;
    memcpy(out.reply, reply.c_str(), rn);
    out.ok = ok;

    xQueueSend(s_llmOut, &out, portMAX_DELAY);
    s_llmBusy = false;
  }
}

// Strip markdown fences/emphasis, code-block content, and multi-byte
// emoji — the LLM occasionally returns long technical replies with big
// ```fenced``` code sections that have no business being read aloud, and
// those sections also blow up the ElevenLabs POST body + LittleFS write
// (which once assert-crashed the board). We keep the original reply for
// the web chat log and only sanitize what feeds the TTS + subtitle.
static String sanitizeForSpeech(const String &in) {
  // Step 1: drop fenced code blocks entirely. Looks for ``` ... ``` and
  // removes the whole region, non-greedy.
  String s = in;
  while (true) {
    int a = s.indexOf("```");
    if (a < 0) break;
    int b = s.indexOf("```", a + 3);
    if (b < 0) { s = s.substring(0, a); break; }     // unterminated: chop
    s = s.substring(0, a) + s.substring(b + 3);
  }

  // Step 2: strip inline backticks but keep the wrapped text.
  s.replace("`", "");

  // Step 3: collapse common markdown emphasis markers.
  s.replace("**", "");
  s.replace("__", "");
  // Solo * and _ are ambiguous (could be bullet points or words). Leave
  // them — ElevenLabs handles them fine.

  // Step 4: keep only characters that are safe for TTS. Drops non-BMP
  // emoji (UTF-8 4-byte sequences start with 0xF0..0xF4) and control
  // characters except \n and \t.
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ) {
    uint8_t c = (uint8_t)s[i];
    if (c >= 0xF0) { i += 4; continue; }    // skip 4-byte emoji
    if (c >= 0xE0) { out += s[i]; out += s[i+1]; out += s[i+2]; i += 3; continue; }
    if (c >= 0xC0) { out += s[i]; out += s[i+1]; i += 2; continue; }
    if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) out += (char)c;
    i += 1;
  }

  // Step 5: collapse runs of whitespace (the markdown strip can leave
  // blank lines behind) and hard-cap at 600 chars for latency + safety.
  String collapsed;
  collapsed.reserve(out.length());
  bool lastWasSpace = false;
  for (size_t i = 0; i < out.length(); ++i) {
    char c = out[i];
    bool isWs = (c == ' ' || c == '\n' || c == '\t' || c == '\r');
    if (isWs) {
      if (!lastWasSpace) collapsed += ' ';
      lastWasSpace = true;
    } else {
      collapsed += c;
      lastWasSpace = false;
    }
  }
  collapsed.trim();
  if (collapsed.length() > 600) collapsed = collapsed.substring(0, 597) + "…";
  return collapsed;
}

static void drainLlmReplies() {
  LlmReply out;
  while (s_llmOut && xQueueReceive(s_llmOut, &out, 0) == pdTRUE) {
    String user  = String(out.user);
    String reply = out.ok ? String(out.reply) : String("I got nothing. Ask me again?");
    Serial.printf("<< daemon: %s\n", reply.c_str());

    // Web chat log gets the full formatted reply.
    serverSetReply(user, reply);

    // Subtitle + TTS get a cleaned version — no code fences, no emoji,
    // length-capped — so long technical answers don't crash the board or
    // make Daemon read "triple backtick bash" out loud.
    String spoken = sanitizeForSpeech(reply);
    creatureSetSubtitle(spoken.length() > 0 ? spoken : reply);
    if (s_voiceOk && spoken.length() > 0) {
      creatureSetMood(MOOD_TALK);
      creatureSetTalking(true);
      serverSetStatus("speaking…");
      voiceSpeak(spoken);    // async itself — returns immediately
    } else {
      creatureSetMood(MOOD_IDLE);
      serverSetStatus("idle");
    }
  }
}

// Screens: creature (home), wallet (swipe-left), settings (pull-down).
enum Screen : uint8_t {
  SCREEN_CREATURE = 0,
  SCREEN_WALLET   = 1,
  SCREEN_SETTINGS = 2,
  SCREEN_WIFI     = 3,   // sub-screen reached from Settings
};
static Screen s_screen     = SCREEN_CREATURE;
static Screen s_prevScreen = SCREEN_CREATURE;   // where to return after settings

static void switchScreen(Screen target) {
  if (target == s_screen) return;
  // Remember the non-settings screen we came from so swipe-up restores it.
  if (target == SCREEN_SETTINGS) s_prevScreen = s_screen;
  s_screen = target;
  switch (target) {
    case SCREEN_CREATURE: creatureRepaint();     break;
    case SCREEN_WALLET:   walletScreenDraw();    break;
    case SCREEN_SETTINGS: settingsScreenDraw();  break;
    case SCREEN_WIFI:     wifiScreenEnter();     break;
  }
}

// ---------------------------------------------------------------------------
// The core "ask Gemini + speak" pipeline.
// ---------------------------------------------------------------------------
static void handleUtterance(const String &user) {
  Serial.print(">> you:    "); Serial.println(user);

  // All the heavy work (LLM round-trip, tool calls, TTS fetch) now runs
  // on dedicated worker tasks, so this function just updates UI state
  // and enqueues the request. Returns in microseconds — the main loop
  // keeps animating / servicing touch / polling the web UI during the
  // 10-30 s background processing.
  creatureSetMood(MOOD_THINK);
  creatureSetSubtitle("thinking…");
  serverSetStatus("thinking…");
  serverSetReply(user, "");     // record user turn, clear any stale reply

  if (!s_llmIn) return;

  LlmReq req;
  memset(&req, 0, sizeof(req));
  size_t n = user.length();
  if (n >= sizeof(req.text)) n = sizeof(req.text) - 1;
  memcpy(req.text, user.c_str(), n);

  // Last-wins: if a previous request is still queued, drop it so the
  // newest utterance takes priority.
  if (uxQueueSpacesAvailable(s_llmIn) == 0) {
    LlmReq drop;
    xQueueReceive(s_llmIn, &drop, 0);
  }
  xQueueSend(s_llmIn, &req, 0);
}

// ---------------------------------------------------------------------------
// Background tickers: SOL price + wallet portfolio refresh.
// ---------------------------------------------------------------------------
static void maybeRefreshPrice() {
  if (!s_wifiOk) return;
  if (millis() < s_nextPriceMs) return;
  s_nextPriceMs = millis() + PRICE_INTERVAL_MS;
  priceRequestRefresh();                          // runs on background task
  creatureSetPrice(priceDisplayString());         // updates next frame
}

static void maybeRefreshWallet() {
  if (!s_wifiOk) return;
  if (millis() < s_nextWalletMs) return;
  s_nextWalletMs = millis() + WALLET_INTERVAL_MS;
  walletRequestRefresh();                         // runs on background task
}

// Scheduled prompt. When heartbeat is toggled on we fire the first run
// immediately (so the user gets instant feedback); subsequent runs are
// spaced by the configured interval. Each tick flows through the normal
// x402 pipeline so the wallet pays for it like any other chat turn.
static void maybeFireHeartbeat() {
  if (!s_wifiOk || !s_voiceOk) return;
  if (voiceIsSpeaking())       return;   // don't stomp on current reply

  bool on = devcfgHeartbeatEnabled();
  if (on && !s_heartbeatWasOn) {
    // Just flipped on — fire immediately by pretending the last run was
    // far enough in the past.
    s_lastHeartbeatMs = 0;
  }
  s_heartbeatWasOn = on;
  if (!on) return;

  uint32_t intervalMs = devcfgHeartbeatIntervalMin() * 60000UL;
  if (intervalMs < 60000UL) intervalMs = 60000UL;   // lower bound: 1 min
  if (s_lastHeartbeatMs != 0 &&
      (millis() - s_lastHeartbeatMs) < intervalMs) return;

  String prompt = devcfgHeartbeatPrompt();
  prompt.trim();
  if (prompt.length() == 0) return;

  Serial.printf(">> heartbeat: %s\n", prompt.c_str());
  s_lastHeartbeatMs = millis();

  // Hand the heartbeat prompt to the LLM worker just like a normal chat
  // so the main loop keeps animating while Gemini thinks.
  handleUtterance(prompt);
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
      if (s_serialBuf.length() > 0) {
        // Dev-only commands. Anything that isn't recognised falls through
        // to the normal chat pipeline.
        if (s_serialBuf == "/ar-test") {
          arweaveSelfTest();
        } else {
          handleUtterance(s_serialBuf);
        }
      }
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

  // Dump the previous reset reason on every boot so when the board
  // mystery-restarts we can see why as soon as it comes back up.
  esp_reset_reason_t rr = esp_reset_reason();
  const char *rrName = "unknown";
  switch (rr) {
    case ESP_RST_POWERON:  rrName = "power-on";            break;
    case ESP_RST_EXT:      rrName = "external reset";      break;
    case ESP_RST_SW:       rrName = "software reset";      break;
    case ESP_RST_PANIC:    rrName = "panic (Guru Meditation / stack / abort)"; break;
    case ESP_RST_INT_WDT:  rrName = "interrupt watchdog";  break;
    case ESP_RST_TASK_WDT: rrName = "task watchdog";       break;
    case ESP_RST_WDT:      rrName = "other watchdog";      break;
    case ESP_RST_DEEPSLEEP:rrName = "wake from deep sleep";break;
    case ESP_RST_BROWNOUT: rrName = "brownout (voltage sag)"; break;
    case ESP_RST_SDIO:     rrName = "sdio";                break;
    default: break;
  }
  Serial.printf("== Daemon booting (prev reset: %s, free heap: %u) ==\n",
                rrName, (unsigned)ESP.getFreeHeap());

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(0x0000);

  // LCD backlight is now driven by PWM from devcfg; applied after voice
  // init so the Audio library exists by the time we call voiceSetVolume.

  if (!creatureBegin(&tft)) {
    Serial.println("creature: begin failed");
    tft.setTextColor(0xF800);
    tft.setCursor(10, 20);
    tft.print("creature sprite alloc failed");
  }
  walletScreenBegin(&tft);
  settingsScreenBegin(&tft);
  wifiScreenBegin(&tft);
  touchBegin();
  creatureSetStatus("WAKING UP");
  creatureTick();   // first frame

  s_voiceOk = voiceBegin();
  Serial.printf("voice: %s\n", s_voiceOk ? "OK" : "failed");

  // LLM worker task — heavy x402 round-trips run here so the main loop
  // stays free to animate + service HTTP + touch during processing.
  s_llmIn  = xQueueCreate(2, sizeof(LlmReq));
  s_llmOut = xQueueCreate(4, sizeof(LlmReply));
  xTaskCreatePinnedToCore(llmWorkerTask, "llm", 16384, nullptr,
                          1, nullptr, 1);

  // Device-level settings (volume, brightness, BT). Applies brightness
  // PWM and restores last-known volume from NVS.
  devcfgBegin();
  creatureSetFaceStyle(devcfgFaceStyle());

  // Wi-Fi + web server. These are best-effort; creature still animates
  // without them. That lets you iterate on the drawing without a network.
  creatureSetStatus("JOINING WIFI");
  creatureTick();
  s_wifiOk = serverBeginWifi();
  if (s_wifiOk) {
    aiBegin();
    serverBeginHttp(handleUtterance);
    String ipLine = "http://" + serverLocalIP();
    // Status-bar real estate now belongs to the USDC balance (updated
    // every frame in loop()); the web UI address stays in serial + the
    // Settings → Wi-Fi row.
    creatureSetStatus("USDC ...");
    serverSetStatus("idle");
    Serial.print("open "); Serial.print(ipLine); Serial.println(" on your phone");
  } else {
    creatureSetStatus("OFFLINE");
  }

  creatureForceBlink();
  creatureSetMood(MOOD_IDLE);

  // Initialise wallet + price so the AI has context from the very first
  // message and the status bar shows the ticker immediately.
  walletBegin();
  priceBegin();
  if (s_wifiOk) {
    priceRefresh();
    creatureSetPrice(priceDisplayString());
    walletRefresh();
  }
  s_nextPriceMs  = millis() + PRICE_INTERVAL_MS;
  s_nextWalletMs = millis() + WALLET_INTERVAL_MS;

  // On-chain memory: derive the AES key from the wallet seed, start the
  // background write task, and — if enabled — restore recent chat history
  // from Solana memos so Daemon remembers across reboots.
  memoryBegin();
  // Recall when either storage backend is on — memoryRecallTurns picks
  // Arweave (fast GraphQL path) if enabled, otherwise Solana memo scan.
  if (s_wifiOk && (devcfgMemoryEnabled() || devcfgArweaveEnabled()) &&
      memoryKeyReady()) {
    creatureSetSubtitle("restoring memory…");
    MemoryTurn loaded[10];
    int n = memoryRecallTurns(loaded, 10);
    if (n > 0) aiLoadHistory(loaded, n);
  }

  // One-shot TTS diagnostic so we can see via serial what ElevenLabs
  // actually returns when POSTed through HTTPClient, and compare against
  // what the patched library sees. Helps isolate streaming bugs.
  if (s_wifiOk) voiceDiagnose();

  // Boot greeting — proves Wi-Fi, Gemini and the speaker are all alive.
  if (s_wifiOk && s_voiceOk) {
    creatureSetMood(MOOD_TALK);
    creatureSetTalking(true);
    creatureSetSubtitle("Daemon online.");
    String hello;
    if (walletPubkey().length()) {
      hello = String("Daemon online. I am your Solana wallet at ") +
              walletPubkey().substring(0, 4) + ". Ask me anything.";
    } else {
      hello = "Daemon online. Configure my wallet key, then talk to me.";
    }
    voiceSpeak(hello);
  }
}

void loop() {
  // HTTP is blocking-per-request, but handleClient() returns quickly if no
  // client is connected, so the animation keeps running.
  if (s_wifiOk) serverLoop();

  // Handle swipes.
  //   SWIPE_LEFT  → wallet (from creature)
  //   SWIPE_RIGHT → creature (from wallet)
  //   SWIPE_DOWN  → open settings (only fires if start was near top edge)
  //   SWIPE_UP    → close settings (back to previous screen)
  SwipeDir sw = touchPoll();
  if (s_screen == SCREEN_SETTINGS) {
    if (sw == SWIPE_UP)                     switchScreen(s_prevScreen);
    if (settingsScreenConsumeClose())       switchScreen(s_prevScreen);
    if (settingsScreenConsumeWifiTap())     switchScreen(SCREEN_WIFI);
  } else if (s_screen == SCREEN_WIFI) {
    // Wi-Fi screen decides itself whether a swipe goes back to the list
    // or exits the panel; we just pipe the swipe through.
    wifiScreenHandleSwipe(sw);
    if (wifiScreenConsumeExit()) switchScreen(SCREEN_SETTINGS);
  } else if (s_screen == SCREEN_WALLET) {
    // Wallet is now a "pull-up" drawer: swipe down to dismiss it back to
    // the creature. We deliberately don't let settings open from here so
    // the close gesture is unambiguous — users return to the creature
    // screen first, then pull settings down from the top edge.
    if (sw == SWIPE_DOWN) switchScreen(SCREEN_CREATURE);
  } else {
    // Creature screen:
    //   SWIPE_LEFT / SWIPE_RIGHT → cycle face style (skin picker)
    //   SWIPE_UP                 → open the wallet drawer
    //   SWIPE_DOWN (top edge)    → open settings
    if (sw == SWIPE_LEFT || sw == SWIPE_RIGHT) {
      int dir = (sw == SWIPE_RIGHT) ? 1 : -1;
      uint8_t next = (uint8_t)((creatureFaceStyle() + DEVCFG_FACE_COUNT + dir)
                               % DEVCFG_FACE_COUNT);
      static const char *kFaceNames[] = { "Daemon", "Robot", "ToyRobot", "Calc" };
      Serial.printf("face: swipe %s -> %u (%s)\n",
                    (sw == SWIPE_RIGHT) ? "right" : "left",
                    (unsigned)next, kFaceNames[next]);
      creatureSetFaceStyle(next);
      devcfgSetFaceStyle(next);
      creatureRepaint();
    }
    if (sw == SWIPE_UP)   switchScreen(SCREEN_WALLET);
    if (sw == SWIPE_DOWN) switchScreen(SCREEN_SETTINGS);
  }

  // Talking state still drives the creature regardless of which screen is
  // visible — audio and mood transitions are shared.
  static bool wasTalking = false;
  bool talking = voiceIsSpeaking();
  creatureSetTalking(talking);
  if (wasTalking && !talking) {
    creatureSetMood(MOOD_IDLE);
    creatureSetSubtitle("");      // clear the reply as soon as audio ends
    serverSetStatus("idle");
  }
  wasTalking = talking;

  // Per-screen tick.
  switch (s_screen) {
    case SCREEN_CREATURE: creatureTick();       break;
    case SCREEN_WALLET:   walletScreenTick();   break;
    case SCREEN_SETTINGS: settingsScreenTick(); break;
    case SCREEN_WIFI:     wifiScreenTick();     break;
  }

  voiceLoop();
  pumpSerialInput();

  // Drain any LLM replies the worker has finished while we were ticking.
  drainLlmReplies();

  // Background tickers — only run when audio is quiet so we don't stutter
  // mid-playback. Wallet/price fetches themselves now live on background
  // tasks (see wallet.cpp / price.cpp) so these are just cheap triggers.
  // Heartbeat runs the full chat pipeline (blocks for seconds) so we also
  // gate it on not-currently-speaking.
  if (!voiceIsSpeaking() && !s_llmBusy) {
    maybeRefreshPrice();
    maybeRefreshWallet();
    maybeFireHeartbeat();
  }

  // Keep the top-left status showing the wallet's current USDC balance.
  // creatureSetStatus is a cheap setter; drawStatusIfChanged only repaints
  // when the string actually differs, so calling it every frame is fine.
  if (s_wifiOk) {
    String usdc = walletUsdcDisplayString();
    if (usdc.length() > 0) creatureSetStatus(usdc);
  }

  // Deadline-based pacing instead of a fixed delay(16): if this iteration
  // took longer than the 16 ms frame budget we sleep less (or not at all)
  // on the next one, keeping animation time-correct even when a tick was
  // heavy (e.g., a swipe that triggered a full redraw).
  static uint32_t s_nextFrameMs = 0;
  uint32_t now = millis();
  if (s_nextFrameMs <= now) s_nextFrameMs = now + 16;
  uint32_t sleep = s_nextFrameMs - now;
  if (sleep > 32) sleep = 32;               // never sleep longer than 2 frames
  s_nextFrameMs += 16;
  delay(sleep < 1 ? 1 : sleep);
}
