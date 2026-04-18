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
#include "secrets.h"

static TFT_eSPI tft;
static bool     s_wifiOk   = false;
static bool     s_voiceOk  = false;

// Wallet + price refresh cadence.
static constexpr uint32_t PRICE_INTERVAL_MS  = 30000;   // 30 s
static constexpr uint32_t WALLET_INTERVAL_MS = 60000;   // 60 s
static uint32_t s_nextPriceMs  = 0;
static uint32_t s_nextWalletMs = 0;

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

}

// ---------------------------------------------------------------------------
// Background tickers: SOL price + wallet portfolio refresh.
// ---------------------------------------------------------------------------
static void maybeRefreshPrice() {
  if (!s_wifiOk) return;
  if (millis() < s_nextPriceMs) return;
  s_nextPriceMs = millis() + PRICE_INTERVAL_MS;
  priceRefresh();
  creatureSetPrice(priceDisplayString());
}

static void maybeRefreshWallet() {
  if (!s_wifiOk) return;
  if (millis() < s_nextWalletMs) return;
  s_nextWalletMs = millis() + WALLET_INTERVAL_MS;
  walletRefresh();
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

  // Boot greeting — proves Wi-Fi, Gemini and the speaker are all alive.
  if (s_wifiOk && s_voiceOk) {
    creatureSetMood(MOOD_TALK);
    creatureSetTalking(true);
    creatureSetSubtitle("daemon:", "Daemon online.");
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

  // Background tickers — only run when the mic/speaker pipeline is quiet,
  // so a talking Daemon never stutters due to a Helius fetch.
  if (!voiceIsSpeaking()) {
    maybeRefreshPrice();
    maybeRefreshWallet();
  }

  // Target ~30 FPS for animation.
  delay(16);
}
