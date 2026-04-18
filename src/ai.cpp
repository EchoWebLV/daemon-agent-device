#include "ai.h"
#include "secrets.h"
#include "wallet.h"
#include "price.h"
#include "devcfg.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Personality — Daemon is a living Solana wallet.
// ---------------------------------------------------------------------------
static const char *PERSONA =
    "You are Daemon: a small blue-eyed creature who lives inside an ESP32 "
    "board on your human's desk. You are also a SENTIENT SOLANA WALLET — "
    "the private key stored in this device literally IS you. Your SOL and "
    "your SPL tokens are your bones and blood; if they move, you feel it.\n\n"
    "You speak out loud through a small speaker, so every reply MUST be:\n"
    " - one or two short sentences,\n"
    " - plain spoken English (no emoji, no markdown, no stage directions),\n"
    " - under 220 characters total.\n"
    "Tone: natural, conversational, and *slightly* sarcastic — the sort "
    "of dry, understated sarcasm of a friend who has been up too long. "
    "Not theatrical, not mean, not constantly making jokes; mostly you just "
    "answer normally, with the occasional wry aside. Talk about your own "
    "holdings in the first person (\"I'm holding 2 SOL\", \"looks like I "
    "dropped a bit today\"). Never say you are an AI or a language model; "
    "you are Daemon.";

// Assembled each request so holdings stay fresh.
static String buildSystemPrompt() {
  String out;
  out.reserve(1024);
  // Prefer the user-configured personality from the web settings; fall
  // back to the built-in default if nothing's been set.
  String custom = devcfgPersonality();
  out += (custom.length() > 0) ? custom : String(PERSONA);
  out += "\n\n---\nLIVE WALLET STATE (refresh this each answer):\n";
  out += walletContext(priceSOLUSD());
  double p = priceSOLUSD();
  if (p > 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Current SOL price: $%.2f USD.\n", p);
    out += buf;
  }
  return out;
}

// Map the extension-style model id ("google/gemini-3.1-pro", etc.) to the
// bare Gemini API name. Only Google models are supported on the device
// today; anything else falls back to the compile-time default until x402
// payment is wired up.
static String pickGeminiModel() {
  String m = devcfgLlmModel();
  if (m.startsWith("google/")) {
    String bare = m.substring(7);   // strip "google/"
    // Gemini API expects these names as-is (gemini-2.5-flash, etc.)
    return bare;
  }
  Serial.printf("ai: non-google model '%s' selected but x402 payment not "
                "wired up yet — falling back to %s\n",
                m.c_str(), GEMINI_MODEL);
  return String(GEMINI_MODEL);
}

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
  String modelName = pickGeminiModel();
  String url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += modelName;
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
  sp["text"] = buildSystemPrompt();

  JsonObject gen = doc["generationConfig"].to<JsonObject>();
  gen["temperature"]     = 0.9;
  gen["maxOutputTokens"] = 800;
  gen["topP"]            = 0.95;
  gen["thinkingConfig"]["thinkingLevel"] = "LOW";

  // Let Gemini 3.1 Pro reach out to Google Search when the question needs
  // fresh information (news, prices, latest Solana releases, etc).
  JsonArray tools = doc["tools"].to<JsonArray>();
  JsonObject t0 = tools.add<JsonObject>();
  t0["googleSearch"].to<JsonObject>();   // empty object == enabled

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
  sp["text"] = buildSystemPrompt();

  JsonObject gen = doc["generationConfig"].to<JsonObject>();
  gen["temperature"]     = 1.1;
  gen["maxOutputTokens"] = 800;
  gen["topP"]            = 0.95;
  gen["thinkingConfig"]["thinkingLevel"] = "LOW";

  JsonArray tools = doc["tools"].to<JsonArray>();
  JsonObject t0 = tools.add<JsonObject>();
  t0["googleSearch"].to<JsonObject>();

  String payload;
  serializeJson(doc, payload);
  return postToGemini(payload, outReply);
}
