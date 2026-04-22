// ============================================================================
//  src/testharness.cpp — see testharness.h header for protocol overview.
// ============================================================================
#include "testharness.h"

static bool   s_testMode = false;
static String s_lineBuf;

// Forward decl; defined below, grows as we add verbs.
static void handleLine(const String &line);

void testHarnessBegin() {
  s_lineBuf.reserve(256);
}

bool testHarnessInTestMode() {
  return s_testMode;
}

void testHarnessTick() {
  // Non-blocking line assembly. Any byte that isn't \r/\n appends to the
  // buffer; \n flushes the buffer through the dispatcher.
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      if (s_lineBuf.length() > 0) {
        handleLine(s_lineBuf);
      }
      s_lineBuf = "";
    } else if (s_lineBuf.length() < 255) {
      s_lineBuf += (char)c;
    }
    // If the buffer would overflow, the extra bytes are silently dropped.
    // The next \n still flushes whatever was captured, so we never wedge.
  }
}

// ---------------------------------------------------------------------------
// Dispatcher. Each verb is one `if (rest.startsWith("..."))` branch.
// Unknown verbs must respond, not hang, so the host's readline() never
// times out on a typo.
// ---------------------------------------------------------------------------
static void handleLine(const String &line) {
  if (!line.startsWith("TEST ")) return;   // not our traffic, ignore
  const String rest = line.substring(5);   // drop the "TEST " prefix

  // --- BEGIN / END / PING ------------------------------------------------
  if (rest == "BEGIN") {
    s_testMode = true;
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

  // --- Unknown verb: always respond -------------------------------------
  Serial.printf("TEST ERR unknown %s\n", rest.c_str());
}
