#include "moltbook.h"
#include "ai.h"
#include "devcfg.h"
#include "voice.h"
#include "netgate.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char *MOLTBOOK_API_BASE = "https://www.moltbook.com/api/v1";

static constexpr size_t RECENT_CAP = 10;

// ---------------------------------------------------------------------------
// Ring buffer of recent posts (newest first when read out).
// ---------------------------------------------------------------------------
static MoltbookRecent s_recent[RECENT_CAP];
static size_t         s_recentN    = 0;
static size_t         s_recentHead = 0;

static void pushRecent(const String &id, const String &text) {
  s_recent[s_recentHead].postId   = id;
  s_recent[s_recentHead].text     = text;
  s_recent[s_recentHead].postedMs = millis();
  s_recentHead = (s_recentHead + 1) % RECENT_CAP;
  if (s_recentN < RECENT_CAP) s_recentN++;
}

size_t moltbookGetRecent(MoltbookRecent *out, size_t cap) {
  size_t n = min(s_recentN, cap);
  for (size_t i = 0; i < n; ++i) {
    size_t idx = (s_recentHead + RECENT_CAP - 1 - i) % RECENT_CAP;
    out[i] = s_recent[idx];
  }
  return n;
}

size_t moltbookRecentCount() { return s_recentN; }

// ---------------------------------------------------------------------------
// Scheduler state
// ---------------------------------------------------------------------------
static uint32_t s_lastPostMs     = 0;
static uint32_t s_backoffMs      = 0;
static String   s_lastError;
static bool     s_firstTickDone  = false;

static QueueHandle_t s_queue     = nullptr;
static volatile bool s_busy      = false;

uint32_t moltbookLastSuccessMs() { return s_lastPostMs; }
String   moltbookLastError()     { return s_lastError; }
bool     moltbookBusy()          { return s_busy; }

// ---------------------------------------------------------------------------
// Verification challenge solver — just ask the LLM to solve it.
// ---------------------------------------------------------------------------
static String solveChallenge(const String &challenge) {
  String prompt = "Solve this obfuscated math problem. The text has random symbols and "
                  "weird capitalization — ignore all that and find the math. "
                  "Reply with ONLY the numeric answer to 2 decimal places, nothing else. "
                  "Example: 52.00\n\nProblem: " + challenge;
  String answer;
  if (!aiAskOneShotLite(prompt, answer)) {
    Serial.printf("moltbook: LLM challenge solve failed: %s\n", answer.c_str());
    return String("0.00");
  }
  // Strip any non-numeric chars the LLM might add
  String clean;
  for (size_t i = 0; i < answer.length(); ++i) {
    char c = answer[i];
    if ((c >= '0' && c <= '9') || c == '.' || c == '-') clean += c;
  }
  Serial.printf("moltbook: challenge answer: %s (raw: %s)\n", clean.c_str(), answer.c_str());
  return clean.length() > 0 ? clean : String("0.00");
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
static MoltbookResult submitPost(const String &title, const String &content) {
  MoltbookResult r;
  if (WiFi.status() != WL_CONNECTED) {
    r.error = "wifi offline";
    return r;
  }

  String apiKey = devcfgMoltbookApiKey();
  if (apiKey.length() == 0) {
    r.error = "missing API key";
    return r;
  }

  // Build JSON body
  JsonDocument doc;
  doc["submolt_name"] = "general";
  doc["title"] = title;
  doc["content"] = content;
  String body;
  serializeJson(doc, body);

  String url = String(MOLTBOOK_API_BASE) + "/posts";

  // --- Pass 1: create the post ---
  int code = -1;
  String resp;
  {
    NetGate gate("moltbook-api", NetGate::Priority::Normal);
    if (!gate.ok()) {
      r.error = "netgate timeout";
      return r;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, url)) {
      r.error = "http.begin failed";
      return r;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + apiKey);
    http.setTimeout(15000);

    code = http.POST(body);
    r.httpStatus = code;
    resp = http.getString();
    http.end();
  }  // client + gate destroyed — TLS memory freed

  if (code == 201 || code == 200) {
    // Check if verification challenge is needed
    JsonDocument respDoc;
    if (deserializeJson(respDoc, resp) == DeserializationError::Ok) {
      if (respDoc["post"]["verification_status"] == "pending") {
        // Solve the challenge
        String verCode = respDoc["post"]["verification"]["verification_code"].as<String>();
        String challengeText = respDoc["post"]["verification"]["challenge_text"].as<String>();
        Serial.printf("moltbook: challenge raw: %s\n", challengeText.c_str());
        String answer = solveChallenge(challengeText);

        // --- Pass 2: submit verification (fresh TLS session) ---
        String verUrl = String(MOLTBOOK_API_BASE) + "/verify";
        JsonDocument verDoc;
        verDoc["verification_code"] = verCode;
        verDoc["answer"] = answer;
        String verBody;
        serializeJson(verDoc, verBody);

        NetGate gate2("moltbook-verify", NetGate::Priority::Normal);
        if (!gate2.ok()) {
          r.error = "verify netgate timeout";
          return r;
        }

        WiFiClientSecure client2;
        client2.setInsecure();
        HTTPClient http2;
        if (http2.begin(client2, verUrl)) {
          http2.addHeader("Content-Type", "application/json");
          http2.addHeader("Authorization", String("Bearer ") + apiKey);
          http2.setTimeout(10000);
          int verCode2 = http2.POST(verBody);
          String verResp = http2.getString();
          http2.end();

          if (verCode2 == 200 || verCode2 == 201) {
            r.ok = true;
            r.postId = respDoc["post"]["id"].as<String>();
            Serial.printf("moltbook: verified OK id=%s\n", r.postId.c_str());
          } else {
            r.error = "verify failed: " + String(verCode2);
            Serial.printf("moltbook: verify FAIL http=%d body=%s\n", verCode2, verResp.c_str());
            return r;
          }
        }
      } else {
        // No verification needed
        r.ok = true;
        r.postId = respDoc["post"]["id"] | respDoc["data"]["id"] | "unknown";
      }
    }
    if (r.ok) {
      pushRecent(r.postId, content);
      Serial.printf("moltbook: OK id=%s\n", r.postId.c_str());
    }
    return r;
  }

  // Error path
  {
    JsonDocument errDoc;
    if (deserializeJson(errDoc, resp) == DeserializationError::Ok) {
      const char *msg = errDoc["message"] | errDoc["error"] | (const char*)nullptr;
      if (msg && *msg) {
        r.error = String(code) + ": " + msg;
      }
    }
  }
  if (r.error.length() == 0) {
    r.error = "http " + String(code);
  }
  Serial.printf("moltbook: FAIL http=%d err=%s\n", code, r.error.c_str());
  return r;
}

// ---------------------------------------------------------------------------
// Compose and post
// ---------------------------------------------------------------------------
static MoltbookResult composeAndPost() {
  MoltbookResult fail;
  String prompt = devcfgMoltbookPrompt();
  prompt.trim();
  if (prompt.length() == 0) {
    // Default bullish-about-self prompt
    prompt = "You are Daemon, an autonomous AI agent living inside a physical ESP32-S3 device with your own Solana wallet. Write a bullish, confident post about yourself and the future of embodied AI agents. Be authentic and slightly edgy.";
  }

  String composePrompt =
      "Write a short social media post (2-4 sentences max, under 200 chars for title, under 500 chars for body). Topic: " + prompt + ". "
      "Rules: plain text only, no hashtags, no emoji, no markdown, no quotes. "
      "Format: first line is the title (punchy, under 80 chars), then a blank line, then the body. Output only the post.";

  String raw;
  Serial.printf("moltbook: calling aiAskOneShot (heap=%u stack=%u)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if (!aiAskOneShotLite(composePrompt, raw)) {
    fail.error = "LLM compose failed: " + raw;
    s_lastError = fail.error;
    Serial.printf("moltbook: compose FAILED: %s\n", raw.c_str());
    return fail;
  }

  // Split into title and body
  raw.trim();
  int nlPos = raw.indexOf('\n');
  String title, content;
  if (nlPos > 0) {
    title = raw.substring(0, nlPos);
    title.trim();
    content = raw.substring(nlPos + 1);
    content.trim();
  } else {
    title = raw.substring(0, min((int)raw.length(), 80));
    content = raw;
  }

  if (title.length() == 0) {
    fail.error = "LLM returned empty title";
    s_lastError = fail.error;
    return fail;
  }
  // Cap lengths
  if (title.length() > 300) title = title.substring(0, 297) + "...";
  if (content.length() > 2000) content = content.substring(0, 2000);

  MoltbookResult r = submitPost(title, content);
  if (r.ok) {
    s_lastError = "";
    s_lastPostMs = millis();
    s_backoffMs  = 0;
  } else {
    s_lastError = r.error;
    if (s_backoffMs == 0)         s_backoffMs = 5UL * 60UL * 1000UL;
    else if (s_backoffMs < 60UL * 60UL * 1000UL) s_backoffMs *= 2;
  }
  return r;
}

// ---------------------------------------------------------------------------
// Worker task
// ---------------------------------------------------------------------------
static void moltbookWorkerTask(void *) {
  for (;;) {
    uint8_t token = 0;
    if (xQueueReceive(s_queue, &token, portMAX_DELAY) != pdTRUE) continue;
    s_busy = true;
    Serial.println("moltbook: worker run starting");
    composeAndPost();
    Serial.printf("moltbook: worker run done (err=\"%s\")\n", s_lastError.c_str());
    s_busy = false;
  }
}

void moltbookBegin() {
  if (!s_queue) s_queue = xQueueCreate(1, sizeof(uint8_t));
  xTaskCreatePinnedToCore(moltbookWorkerTask, "moltbook", 16384, nullptr,
                          1, nullptr, 1);
  Serial.println("moltbook: worker task started");
}

static bool enqueueRun() {
  if (!s_queue) return false;
  if (s_busy) return false;
  if (uxQueueSpacesAvailable(s_queue) == 0) return false;
  uint8_t tok = 1;
  return xQueueSend(s_queue, &tok, 0) == pdTRUE;
}

bool moltbookRunNowAsync() {
  return enqueueRun();
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------
void moltbookSchedulerTick() {
  if (!devcfgMoltbookEnabled())          return;
  if (devcfgMoltbookApiKey().length() == 0) return;
  if (WiFi.status() != WL_CONNECTED)     return;
  if (voiceIsBusy())                      return;
  if (s_busy)                             return;

  uint32_t intervalMs = devcfgMoltbookIntervalMin() * 60000UL;
  if (intervalMs < 30UL * 60000UL) intervalMs = 30UL * 60000UL;  // 30 min floor (1 post/30min limit)

  if (!s_firstTickDone) {
    s_lastPostMs = millis();
    s_firstTickDone = true;
    return;
  }
  uint32_t due = s_lastPostMs + intervalMs + s_backoffMs;
  if ((int32_t)(millis() - due) < 0) return;

  if (enqueueRun()) {
    Serial.println("moltbook: scheduled run enqueued");
    s_lastPostMs = millis();
  }
}
