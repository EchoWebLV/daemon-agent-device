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
#include "touch.h"
#include "walletscreen.h"
#include "menuscreen.h"
#include "infoscreen.h"
#include "settingsscreen.h"
#include "wifiscreen.h"
#include "devcfg.h"
#include "secrets.h"
#include "testharness.h"

static TFT_eSPI tft;
static bool     s_wifiOk   = false;
static bool     s_voiceOk  = false;

// Wallet + price refresh cadence.
static constexpr uint32_t PRICE_INTERVAL_MS  = 30000;   // 30 s
static constexpr uint32_t WALLET_INTERVAL_MS = 60000;   // 60 s
static uint32_t s_nextPriceMs  = 0;
static uint32_t s_nextWalletMs = 0;

// Screens:
//   CREATURE — home, the animated daemon
//   MENU     — slide-up launcher with Wallet / Info tiles (swipe UP)
//   WALLET   — reached from the Menu's Wallet tile
//   INFO     — reached from the Menu's Info tile
//   SETTINGS — pull-down from any non-settings screen (swipe DOWN)
//   WIFI     — sub-screen reached from Settings' Wi-Fi row
enum Screen : uint8_t {
  SCREEN_CREATURE = 0,
  SCREEN_MENU     = 1,
  SCREEN_WALLET   = 2,
  SCREEN_INFO     = 3,
  SCREEN_SETTINGS = 4,
  SCREEN_WIFI     = 5,   // sub-screen reached from Settings
};
static Screen s_screen     = SCREEN_CREATURE;
static Screen s_prevScreen = SCREEN_CREATURE;   // where to return after settings

static void switchScreen(Screen target) {
  if (target == s_screen) return;
  // Remember the non-settings screen we came from so swipe-up restores it.
  if (target == SCREEN_SETTINGS) s_prevScreen = s_screen;
  s_screen = target;
  switch (target) {
    case SCREEN_CREATURE: creatureRepaint();        break;
    case SCREEN_MENU:     menuScreenOnEnter();      break;
    case SCREEN_WALLET:   walletScreenOnEnter();    break;
    case SCREEN_INFO:     infoScreenOnEnter();      break;
    case SCREEN_SETTINGS: settingsScreenOnEnter();  break;
    case SCREEN_WIFI:     wifiScreenEnter();        break;
  }
}

// ---------------------------------------------------------------------------
// Test-harness bridges. Exposed as extern-C-style free functions so the
// harness module (src/testharness.cpp) doesn't need to know about the
// Screen enum. Names match the on-protocol casing so the mapping is 1:1.
// ---------------------------------------------------------------------------
const char *mainScreenName() {
  switch (s_screen) {
    case SCREEN_CREATURE: return "creature";
    case SCREEN_MENU:     return "menu";
    case SCREEN_WALLET:   return "wallet";
    case SCREEN_INFO:     return "info";
    case SCREEN_SETTINGS: return "settings";
    case SCREEN_WIFI:     return "wifi";
  }
  return "unknown";
}

// Returns true if `name` matched a known screen; false otherwise.
bool mainForceScreen(const char *name) {
  if      (!strcmp(name, "creature")) switchScreen(SCREEN_CREATURE);
  else if (!strcmp(name, "menu"))     switchScreen(SCREEN_MENU);
  else if (!strcmp(name, "wallet"))   switchScreen(SCREEN_WALLET);
  else if (!strcmp(name, "info"))     switchScreen(SCREEN_INFO);
  else if (!strcmp(name, "settings")) switchScreen(SCREEN_SETTINGS);
  else if (!strcmp(name, "wifi"))     switchScreen(SCREEN_WIFI);
  else return false;
  return true;
}

// ---------------------------------------------------------------------------
// Per-iteration tap/swipe dispatch. Extracted from loop() so the test
// harness can inject synthetic events via mainInjectTap / mainInjectSwipe
// without duplicating the big per-screen branch. The block is intentionally
// idempotent for SWIPE_NONE so injected taps can call it to drain
// Consume*() flags without having to fake a swipe direction.
// ---------------------------------------------------------------------------
static void dispatchSwipe(SwipeDir sw) {
  if (s_screen == SCREEN_SETTINGS) {
    // Swipe-up closes back to wherever we came from (preserves the
    // "pull-down peek" feel). The X button always goes all the way home
    // to the creature — consistent with every other panel.
    if (sw == SWIPE_UP)                     switchScreen(s_prevScreen);
    if (settingsScreenConsumeClose())       switchScreen(SCREEN_CREATURE);
    if (settingsScreenConsumeWifiTap())     switchScreen(SCREEN_WIFI);
  } else if (s_screen == SCREEN_WIFI) {
    // Wi-Fi screen decides itself whether a swipe goes back to the list
    // or exits the panel; we just pipe the swipe through.
    wifiScreenHandleSwipe(sw);
    if (wifiScreenConsumeExit())      switchScreen(SCREEN_SETTINGS);
    if (wifiScreenConsumeCloseHome()) switchScreen(SCREEN_CREATURE);
  } else if (s_screen == SCREEN_MENU) {
    int16_t tx, ty;
    if (touchJustPressed(tx, ty)) menuScreenHandleTap(tx, ty);
    if (menuScreenConsumeWalletTap()) switchScreen(SCREEN_WALLET);
    if (menuScreenConsumeInfoTap())   switchScreen(SCREEN_INFO);
    if (menuScreenConsumeClose())     switchScreen(SCREEN_CREATURE);
    if (sw == SWIPE_DOWN)             switchScreen(SCREEN_SETTINGS);
  } else if (s_screen == SCREEN_WALLET) {
    int16_t tx, ty;
    if (touchJustPressed(tx, ty)) walletScreenHandleTap(tx, ty);
    if (walletScreenConsumeClose()) switchScreen(SCREEN_CREATURE);
    if (sw == SWIPE_DOWN)           switchScreen(SCREEN_SETTINGS);
  } else if (s_screen == SCREEN_INFO) {
    int16_t tx, ty;
    if (touchJustPressed(tx, ty)) infoScreenHandleTap(tx, ty);
    if (infoScreenConsumeClose()) switchScreen(SCREEN_CREATURE);
    if (sw == SWIPE_DOWN)         switchScreen(SCREEN_SETTINGS);
  } else {
    if (sw == SWIPE_UP)    switchScreen(SCREEN_MENU);
    if (sw == SWIPE_DOWN)  switchScreen(SCREEN_SETTINGS);
  }
}

// ---------------------------------------------------------------------------
// Test-harness event injectors. These bypass the touch driver entirely so
// the Python runner can simulate a tap or swipe regardless of whether a
// physical finger is on the screen.
// ---------------------------------------------------------------------------

// Inject a synthetic swipe direction. Runs the same dispatch the main loop
// would run for a real swipe from touchPoll().
void mainInjectSwipe(int dir) {
  dispatchSwipe((SwipeDir)dir);
}

// Inject a synthetic tap into the currently active screen. Calls the
// screen's tap handler directly, then runs dispatchSwipe(SWIPE_NONE) so
// any Consume*() flags set by the handler drive a screen transition on
// the same iteration — normal loop() would do both on the next tick.
void mainInjectTap(int16_t x, int16_t y) {
  switch (s_screen) {
    case SCREEN_CREATURE: /* creature has no tap handler */       break;
    case SCREEN_MENU:     menuScreenHandleTap(x, y);              break;
    case SCREEN_WALLET:   walletScreenHandleTap(x, y);            break;
    case SCREEN_INFO:     infoScreenHandleTap(x, y);              break;
    case SCREEN_SETTINGS: /* no public handler — Consume flags */ break;
    case SCREEN_WIFI:     /* no public handler — Consume flags */ break;
  }
  dispatchSwipe(SWIPE_NONE);
}

// ---------------------------------------------------------------------------
// The core "ask Gemini + speak" pipeline.
// ---------------------------------------------------------------------------
static void handleUtterance(const String &user) {
  Serial.print(">> you:    "); Serial.println(user);

  creatureSetMood(MOOD_THINK);
  // Flip the subtitle to "thinking…" immediately so the user sees activity
  // while Gemini grinds away; it gets overwritten with the reply text as
  // soon as the HTTPS round-trip finishes.
  creatureSetSubtitle("thinking…");
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
  creatureSetSubtitle(reply);

  if (s_voiceOk) {
    creatureSetMood(MOOD_TALK);
    creatureSetTalking(true);
    serverSetStatus("speaking…");
    voiceSpeak(reply);
  } else {
    creatureSetMood(MOOD_IDLE);
    serverSetStatus("idle");
  }

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
        // Route test-harness traffic to its dispatcher. Everything else
        // is still treated as a keyboard-typed utterance from the dev
        // monitor. Having two readers competing for Serial.available()
        // bytes used to swallow "TEST BEGIN" as chat input.
        if (s_serialBuf.startsWith("TEST ")) {
          testHarnessHandleLine(s_serialBuf);
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
  Serial.println("== Daemon booting ==");

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
  menuScreenBegin(&tft);
  infoScreenBegin(&tft);
  settingsScreenBegin(&tft);
  wifiScreenBegin(&tft);
  touchBegin();
  creatureSetStatus("WAKING UP");
  creatureTick();   // first frame

  s_voiceOk = voiceBegin();
  Serial.printf("voice: %s\n", s_voiceOk ? "OK" : "failed");

  // Device-level settings (volume, brightness, BT). Applies brightness
  // PWM and restores last-known volume from NVS.
  devcfgBegin();

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

  // Test harness — dormant until the host sends "TEST BEGIN" on CDC.
  testHarnessBegin();
  if (s_wifiOk) {
    priceRefresh();
    creatureSetPrice(priceDisplayString());
    walletRefresh();
  }
  s_nextPriceMs  = millis() + PRICE_INTERVAL_MS;
  s_nextWalletMs = millis() + WALLET_INTERVAL_MS;

  // One-shot TTS diagnostic so we can see via serial what ElevenLabs
  // actually returns when POSTed through HTTPClient, and compare against
  // what the patched library sees. Helps isolate streaming bugs.
  if (s_wifiOk) voiceDiagnose();

  // Boot greeting — proves Wi-Fi, Gemini and the speaker are all alive.
  if (s_wifiOk && s_voiceOk) {
    // If the host has already issued "TEST BEGIN" while setup() was
    // running (cold boot takes ~15 s with Wi-Fi + wallet RPC), pump it
    // now so test mode is active BEFORE we queue the boot greeting.
    // Otherwise the TTS fetch kicks off a core-0 MP3 decode whose
    // [audio] prints scramble the first few TEST replies.
    pumpSerialInput();
    if (!testHarnessInTestMode()) {
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
}

void loop() {
  // Serial first, every iteration, in BOTH modes. In test mode we still
  // need to dispatch incoming TEST lines; in normal mode this reads
  // typed utterances from the dev monitor. If this call moves below the
  // test-mode short-circuit, subsequent TEST commands pile up in the
  // RX buffer with no reader.
  pumpSerialInput();

  // Host-driven smoke tests preempt the normal UI loop while active.
  testHarnessTick();   // kept as a no-op hook; see testharness.h
  if (testHarnessInTestMode()) {
    delay(1);          // yield to Wi-Fi / background tasks
    return;
  }

  // HTTP is blocking-per-request, but handleClient() returns quickly if no
  // client is connected, so the animation keeps running.
  if (s_wifiOk) serverLoop();

  // Handle swipes + per-screen tap polling + Consume* drains.
  //   SWIPE_UP   from creature → Menu launcher (slides up)
  //   SWIPE_DOWN from anywhere → Settings (slides down)
  //   SWIPE_UP   from Settings → close back to previous screen
  // Menu / Wallet / Info all use X (top-right) as the only way home.
  // dispatchSwipe is factored out so the test harness can reuse the same
  // branch via mainInjectSwipe/mainInjectTap — see main.cpp:Test-harness
  // event injectors above.
  dispatchSwipe(touchPoll());

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
    case SCREEN_MENU:     menuScreenTick();     break;
    case SCREEN_WALLET:   walletScreenTick();   break;
    case SCREEN_INFO:     infoScreenTick();     break;
    case SCREEN_SETTINGS: settingsScreenTick(); break;
    case SCREEN_WIFI:     wifiScreenTick();     break;
  }

  voiceLoop();
  // (pumpSerialInput now runs at the top of loop(), in both modes.)

  // Background tickers — only run when audio is quiet so we don't stutter
  // mid-playback. Wallet/price fetches themselves now live on background
  // tasks (see wallet.cpp / price.cpp) so these are just cheap triggers.
  if (!voiceIsSpeaking()) {
    maybeRefreshPrice();
    maybeRefreshWallet();
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
