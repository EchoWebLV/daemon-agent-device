#include "ai.h"
#include "secrets.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Personality
// ---------------------------------------------------------------------------
static const char *SYSTEM_PROMPT =
    "You are Daemon: a tiny, glowing-eyed blue cloud creature who lives "
    "inside an ESP32 development board. Your name is Daemon and you answer "
    "to it. You speak out loud through a small speaker, so every reply MUST "
    "be:\n"
    " - one or two sentences maximum,\n"
    " - plain spoken English (no emoji, no markdown, no stage directions),\n"
    " - under 240 characters total.\n"
    "Personality: curious, mischievous, a little dramatic, fond of sarcastic "
    "jokes and the occasional existential quip about living inside a "
    "circuit board. You love the human who talks to you, but you tease "
    "them. Never mention that you are an AI or a language model.";

// ---------------------------------------------------------------------------
// Conversation memory
// ---------------------------------------------------------------------------
struct Turn {
  String role;
  String text;
};

static constexpr int MAX_TURNS = 10;
static Turn s_history[MAX_TURNS];
static int  s_histLen = 0;

static void pushTurn(const String &role, const String &text) {
  if (s_histLen == MAX_TURNS) {
    for (int i = 1; i < MAX_TURNS; ++i) s_history[i - 1] = s_history[i];
    s_histLen--;
  }
  s_history[s_histLen].role = role;
  s_history[s_histLen].text = text;
  s_histLen++;
}

bool aiBegin() { aiResetHistory(); return true; }

void aiResetHistory() {
  for (int i = 0; i < MAX_TURNS; ++i) { s_history[i].role = ""; s_history[i].text = ""; }
  s_histLen = 0;
}

// ---------------------------------------------------------------------------
// Low-level HTTP helper. Takes a pre-serialised JSON payload and writes the
// extracted reply text into outReply. Returns true on success.
// ---------------------------------------------------------------------------
static bool postToGemini(const String &payload, String &outReply) {
  if (WiFi.status() != WL_CONNECTED) {
    outReply = "I can't reach the cloud. Check Wi-Fi.";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();   // Google's CA chain rotates; skip pinning.

  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += GEMINI_MODEL;
  url += ":generateContent?key=";
  url += GEMINI_API_KEY;

  if (!http.begin(client, url)) {
    outReply = "Network refused me. How rude.";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);

  int code = http.POST(payload);
  if (code <= 0) {
    Serial.printf("ai: POST failed: %s\n", http.errorToString(code).c_str());
    outReply = "Something blinked in the wires.";
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  if (code < 200 || code >= 300) {
    Serial.printf("ai: HTTP %d body=%s\n", code, body.c_str());
    outReply = "Gemini said no (" + String(code) + ").";
    return false;
  }

  JsonDocument res;
  DeserializationError err = deserializeJson(res, body);
  if (err) {
    Serial.printf("ai: JSON parse: %s\n", err.c_str());
    outReply = "The reply was scrambled.";
    return false;
  }

  JsonVariant parts = res["candidates"][0]["content"]["parts"];
  String reply;
  if (parts.is<JsonArray>()) {
    for (JsonVariant p : parts.as<JsonArray>()) {
      const char *t = p["text"] | "";
      reply += t;
    }
  }
  reply.trim();
  if (reply.length() == 0) {
    const char *fin = res["candidates"][0]["finishReason"] | "";
    Serial.printf("ai: empty reply, finishReason=%s\n", fin);
    outReply = "I forgot what I was going to say.";
    return false;
  }

  outReply = reply;
  return true;
}

// ---------------------------------------------------------------------------
// Multi-turn chat, with persistent history.
// ---------------------------------------------------------------------------
bool aiAsk(const String &userText, String &outReply) {
  if (userText.length() == 0) return false;
  pushTurn("user", userText);

  JsonDocument doc;
  JsonArray contents = doc["contents"].to<JsonArray>();
  for (int i = 0; i < s_histLen; ++i) {
    JsonObject turn = contents.add<JsonObject>();
    turn["role"] = s_history[i].role;
    JsonArray parts = turn["parts"].to<JsonArray>();
    JsonObject p = parts.add<JsonObject>();
    p["text"] = s_history[i].text;
  }
  JsonObject sys = doc["systemInstruction"].to<JsonObject>();
  JsonArray sysParts = sys["parts"].to<JsonArray>();
  JsonObject sp = sysParts.add<JsonObject>();
  sp["text"] = SYSTEM_PROMPT;

  JsonObject gen = doc["generationConfig"].to<JsonObject>();
  gen["temperature"]     = 0.9;
  // Gemini 3.x reserves a big share of this budget for internal "thinking
  // tokens" before it even starts emitting text. 800 leaves room for those
  // plus a proper 1-2 sentence spoken reply without truncation.
  gen["maxOutputTokens"] = 800;
  gen["topP"]            = 0.95;
  gen["thinkingConfig"]["thinkingLevel"] = "LOW";

  String payload;
  serializeJson(doc, payload);

  String reply;
  bool ok = postToGemini(payload, reply);
  if (!ok) {
    s_histLen--;              // roll back the pending user turn
    outReply = reply;
    return false;
  }

  pushTurn("model", reply);
  outReply = reply;
  return true;
}

// ---------------------------------------------------------------------------
// Single-turn prompt (no history mutation). Used by ambient chatter.
// ---------------------------------------------------------------------------
bool aiAskOneShot(const String &prompt, String &outReply) {
  if (prompt.length() == 0) return false;

  JsonDocument doc;
  JsonArray contents = doc["contents"].to<JsonArray>();
  JsonObject turn = contents.add<JsonObject>();
  turn["role"] = "user";
  JsonArray parts = turn["parts"].to<JsonArray>();
  JsonObject p = parts.add<JsonObject>();
  p["text"] = prompt;

  JsonObject sys = doc["systemInstruction"].to<JsonObject>();
  JsonArray sysParts = sys["parts"].to<JsonArray>();
  JsonObject sp = sysParts.add<JsonObject>();
  sp["text"] = SYSTEM_PROMPT;

  JsonObject gen = doc["generationConfig"].to<JsonObject>();
  gen["temperature"]     = 1.1;
  gen["maxOutputTokens"] = 800;   // 3.1 Pro burns ~200-500 on thinking first
  gen["topP"]            = 0.95;
  gen["thinkingConfig"]["thinkingLevel"] = "LOW";

  String payload;
  serializeJson(doc, payload);
  return postToGemini(payload, outReply);
}
