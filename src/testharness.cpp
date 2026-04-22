// ============================================================================
//  src/testharness.cpp — see testharness.h header for protocol overview.
// ============================================================================
#include "testharness.h"

#include <WiFi.h>    // status + scan for TEST WIFI STATUS / SCAN

#include "creature.h"
#include "menuscreen.h"
#include "walletscreen.h"
#include "infoscreen.h"
#include "settingsscreen.h"
#include "wifiscreen.h"
#include "touch.h"   // SWIPE_* enum values for TEST SWIPE
#include "wallet.h"  // walletPubkey / walletSolBalance / walletUsdcAmount
#include "ai.h"      // aiAskOneShot for TEST AI PING
#include "x402.h"    // x402Get / x402Post for TEST X402 CALL
#include "voice.h"   // voiceStop() so TEST BEGIN silences the audio task

static bool s_testMode = false;

// Implemented in main.cpp — see "Test-harness bridges" block.
extern const char *mainScreenName();
extern bool        mainForceScreen(const char *name);
extern void        mainInjectTap(int16_t x, int16_t y);
extern void        mainInjectSwipe(int dir);

void testHarnessBegin() {
  // Nothing to do: line assembly happens in main.cpp's pumpSerialInput().
}

bool testHarnessInTestMode() {
  return s_testMode;
}

void testHarnessTick() {
  // See the header: serial reading lives in main.cpp now. This shim
  // exists so any older loop() wiring that still calls testHarnessTick()
  // continues to compile.
}

// ---------------------------------------------------------------------------
// Dispatcher. Each verb is one `if (rest.startsWith("..."))` branch.
// Unknown verbs must respond, not hang, so the host's readline() never
// times out on a typo.
// ---------------------------------------------------------------------------
void testHarnessHandleLine(const String &line) {
  if (!line.startsWith("TEST ")) return;   // not our traffic, ignore
  const String rest = line.substring(5);   // drop the "TEST " prefix

  // --- BEGIN / END / PING ------------------------------------------------
  if (rest == "BEGIN") {
    s_testMode = true;
    // Kill any in-flight MP3 playback so the audio task's Serial prints
    // (which the library reroutes to Serial from its own core) stop
    // splicing into our single-line replies. The s_testMode flag also
    // gates the audio_* callbacks in voice.cpp, so even if a new clip
    // is queued accidentally, it stays silent on the wire.
    voiceStop();
    Serial.println("TEST OK begin");
    return;
  }
  if (rest == "END") {
    s_testMode = false;
    Serial.println("TEST OK end");
    return;
  }
  if (rest == "PING") {
    Serial.printf("TEST OK ping %u\n", (unsigned)millis());
    return;
  }

  // --- HEAP / VERSION ---------------------------------------------------
  if (rest == "HEAP") {
    Serial.printf("TEST OK heap %u %u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram());
    return;
  }
  if (rest == "VERSION") {
    // SDK + build timestamp. Keep build_date as a single token — the
    // protocol is space-delimited, so "Apr 22 2026" becomes "Apr_22_2026".
    String buildDate = __DATE__;  buildDate.replace(' ', '_');
    String buildTime = __TIME__;
    Serial.printf("TEST OK version %s %s %s\n",
                  ESP.getSdkVersion(),
                  buildDate.c_str(),
                  buildTime.c_str());
    return;
  }

  // --- SCREEN GET / FORCE / PAINT ---------------------------------------
  if (rest == "SCREEN GET") {
    Serial.printf("TEST OK screen %s\n", mainScreenName());
    return;
  }
  if (rest.startsWith("SCREEN FORCE ")) {
    String name = rest.substring(strlen("SCREEN FORCE "));
    if (!mainForceScreen(name.c_str())) {
      Serial.printf("TEST ERR screen unknown %s\n", name.c_str());
      return;
    }
    Serial.printf("TEST OK screen %s\n", mainScreenName());
    return;
  }
  if (rest == "SCREEN PAINT") {
    uint32_t t0 = millis();
    // Re-enter the current screen's Draw path to force a repaint of the
    // body. Avoids the ~220 ms slide-in: we call Draw() directly.
    const char *cur = mainScreenName();
    if      (!strcmp(cur, "creature")) creatureRepaint();
    else if (!strcmp(cur, "menu"))     menuScreenDraw();
    else if (!strcmp(cur, "wallet"))   walletScreenDraw();
    else if (!strcmp(cur, "info"))     infoScreenDraw();
    else if (!strcmp(cur, "settings")) settingsScreenDraw();
    else if (!strcmp(cur, "wifi"))     wifiScreenDraw();
    Serial.printf("TEST OK paint %u\n", (unsigned)(millis() - t0));
    return;
  }

  // --- TAP / SWIPE ------------------------------------------------------
  if (rest.startsWith("TAP ")) {
    int x = 0, y = 0;
    if (sscanf(rest.c_str(), "TAP %d %d", &x, &y) == 2) {
      mainInjectTap((int16_t)x, (int16_t)y);
      Serial.println("TEST OK tap");
    } else {
      Serial.println("TEST ERR tap bad_args");
    }
    return;
  }
  if (rest.startsWith("SWIPE ")) {
    String dir = rest.substring(6);
    int d = 0;
    if      (dir == "LEFT")  d = SWIPE_LEFT;
    else if (dir == "RIGHT") d = SWIPE_RIGHT;
    else if (dir == "UP")    d = SWIPE_UP;
    else if (dir == "DOWN")  d = SWIPE_DOWN;
    else {
      Serial.printf("TEST ERR swipe bad_dir %s\n", dir.c_str());
      return;
    }
    mainInjectSwipe(d);
    Serial.println("TEST OK swipe");
    return;
  }

  // --- WIFI STATUS / SCAN -----------------------------------------------
  if (rest == "WIFI STATUS") {
    if (WiFi.status() == WL_CONNECTED) {
      // SSIDs may contain spaces. Replace with underscores so the host's
      // space-delimited parser sees a single token. The host reverses it.
      String ssid = WiFi.SSID();
      ssid.replace(' ', '_');
      Serial.printf("TEST OK wifi connected %s %d\n",
                    ssid.c_str(), (int)WiFi.RSSI());
    } else {
      Serial.println("TEST OK wifi disconnected - 0");
    }
    return;
  }
  if (rest == "WIFI SCAN") {
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false, true);   // sync, include hidden
    if (n < 0) {
      Serial.printf("TEST ERR wifi scan rc=%d\n", n);
      return;
    }
    Serial.printf("TEST OK wifi scan %d\n", n);
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      ssid.replace(' ', '_');
      const char *enc = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "WPA";
      Serial.printf("TEST NET %s %d %s\n",
                    ssid.c_str(), (int)WiFi.RSSI(i), enc);
    }
    WiFi.scanDelete();
    return;
  }

  // --- WALLET PUBKEY / BALANCE ------------------------------------------
  if (rest == "WALLET PUBKEY") {
    const String pk = walletPubkey();
    if (pk.length() == 0) {
      Serial.println("TEST ERR wallet no_pubkey");
    } else {
      Serial.printf("TEST OK pubkey %s\n", pk.c_str());
    }
    return;
  }
  if (rest == "WALLET BALANCE") {
    // Force a fresh refresh so the host isn't reading a minute-old cache.
    walletRefresh();
    Serial.printf("TEST OK balance %.9f %.6f\n",
                  walletSolBalance(), walletUsdcAmount());
    return;
  }

  // --- X402 CALL --------------------------------------------------------
  if (rest.startsWith("X402 CALL ")) {
    String url = rest.substring(strlen("X402 CALL "));
    static uint32_t callSeq = 0;
    ++callSeq;
    uint32_t t0 = millis();
    X402Result r;
    // The LLM endpoint only accepts POST with an OpenAI-style chat body.
    // Everything else goes through GET. We send a tiny unique prompt each
    // call so the paid path actually differs (fresh blockhash per tx —
    // see MEMORY.md on the blockhash-reuse regression).
    if (url.indexOf("/chat/completions") >= 0) {
      String body;
      body.reserve(256);
      body  = "{\"model\":\"gemini-2.5-flash\",";
      body += "\"messages\":[{\"role\":\"user\",\"content\":\"ping ";
      body += String(callSeq);
      body += "\"}]}";
      r = x402Post(url, body);
    } else if (url.indexOf("/api/call") >= 0) {
      // Shannon-style paid endpoint (GET/POST /api/call). Sends the
      // minimal JSON body the server documents; the ping-<seq> token
      // keeps every call's payload distinct so the x402 layer signs a
      // fresh blockhash each time (same rationale as the chat case —
      // see memory/MEMORY.md).
      String body;
      body.reserve(64);
      body  = "{\"message\":\"ping ";
      body += String(callSeq);
      body += "\"}";
      r = x402Post(url, body);
    } else {
      r = x402Get(url);
    }
    uint32_t dt = millis() - t0;
    if (r.status == 0) {
      Serial.printf("TEST ERR x402 %s dt=%u\n",
                    r.error.length() ? r.error.c_str() : "unknown",
                    (unsigned)dt);
      return;
    }
    // paid_usdc_base = 6-decimal USDC base units (e.g. 50000 = $0.05).
    uint64_t paid_base = (uint64_t)llround(r.costUsd * 1000000.0);
    Serial.printf("TEST OK x402 %d %llu %u\n",
                  r.status, (unsigned long long)paid_base, (unsigned)dt);
    return;
  }

  // --- AI PING ----------------------------------------------------------
  if (rest == "AI PING") {
    uint32_t t0 = millis();
    String reply;
    // aiAskOneShot doesn't touch chat history — cleaner for a smoke test.
    bool ok = aiAskOneShot("ping", reply);
    uint32_t dt = millis() - t0;
    if (!ok) {
      Serial.printf("TEST ERR ai no_response dt=%u\n", (unsigned)dt);
      return;
    }
    // The ai module doesn't bubble up the HTTP status; protocol keeps the
    // slot so 200 is a success sentinel and non-200 can mean something
    // specific later.
    Serial.printf("TEST OK ai 200 %u\n", (unsigned)dt);
    return;
  }

  // --- Unknown verb: always respond -------------------------------------
  Serial.printf("TEST ERR unknown %s\n", rest.c_str());
}
