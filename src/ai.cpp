#include "ai.h"
#include "secrets.h"
#include "wallet.h"
#include "price.h"
#include "devcfg.h"
#include "x402.h"

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

// (Gemini-direct routing helper has been retired — all chat now goes
// through sol.blockrun.ai via x402 USDC payment.)

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
// sol.blockrun.ai HTTP gateway — OpenAI-compatible /v1/chat/completions
// behind an x402 USDC paywall on Solana mainnet. All chat calls flow
// through here so the device actually pays for each reply from the wallet,
// matching the Chrome extension's behaviour exactly.
// ---------------------------------------------------------------------------
static const char *LLM_ENDPOINT = "https://sol.blockrun.ai/api/v1/chat/completions";

// Build the OpenAI-style request body. `historyTurns` expects turns in the
// Gemini format we already use (role=="user"/"model"); we translate them
// to OpenAI roles on the fly.
static String buildChatBody(const String &model,
                            const Turn *historyTurns,
                            int historyLen,
                            const String &oneShotPrompt,
                            int maxTokens,
                            float temperature) {
  JsonDocument doc;
  doc["model"] = model;
  doc["max_tokens"] = maxTokens;
  doc["temperature"] = temperature;

  JsonArray msgs = doc["messages"].to<JsonArray>();

  // System prompt (personality + live wallet context).
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"]    = "system";
  sys["content"] = buildSystemPrompt();

  if (historyTurns && historyLen > 0) {
    for (int i = 0; i < historyLen; ++i) {
      JsonObject m = msgs.add<JsonObject>();
      // Gemini's "model" role → OpenAI's "assistant".
      m["role"]    = (historyTurns[i].role == "model") ? "assistant" : "user";
      m["content"] = historyTurns[i].text;
    }
  } else if (oneShotPrompt.length() > 0) {
    JsonObject m = msgs.add<JsonObject>();
    m["role"]    = "user";
    m["content"] = oneShotPrompt;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// POSTs the chat completion through x402 and returns the assistant's reply
// text. Fills outReply with a human-friendly error string on failure.
static bool postChatThroughX402(const String &body, String &outReply) {
  X402Result r = x402Post(String(LLM_ENDPOINT), body);
  if (r.status != 200) {
    Serial.printf("ai: x402 POST %d (err=%s)\n", r.status, r.error.c_str());
    if (r.status == 402) {
      outReply = "I couldn't complete the USDC payment for this reply.";
    } else if (r.error.length() > 0) {
      outReply = "Daemon's brain is offline: " + r.error;
    } else {
      outReply = "HTTP " + String(r.status);
    }
    return false;
  }
  if (r.costUsd > 0) {
    Serial.printf("ai: paid $%.5f USDC for this reply\n", r.costUsd);
  }

  JsonDocument res;
  if (deserializeJson(res, r.body)) {
    outReply = "The reply was scrambled.";
    return false;
  }
  const char *content = res["choices"][0]["message"]["content"] | "";
  String reply = String(content);
  reply.trim();
  if (reply.length() == 0) {
    const char *finish = res["choices"][0]["finish_reason"] | "";
    Serial.printf("ai: empty reply, finish_reason=%s\n", finish);
    outReply = "I forgot what I was going to say.";
    return false;
  }
  outReply = reply;
  return true;
}

// ---------------------------------------------------------------------------
// Multi-turn chat — goes through sol.blockrun.ai with x402 USDC payment.
// ---------------------------------------------------------------------------
bool aiAsk(const String &userText, String &outReply) {
  if (userText.length() == 0) return false;
  pushTurn("user", userText);

  String body = buildChatBody(devcfgLlmModel(),
                              s_history, s_histLen,
                              String(),
                              /*maxTokens=*/ 512,
                              /*temperature=*/ 0.9f);

  String reply;
  bool ok = postChatThroughX402(body, reply);
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
// Single-turn prompt (no history mutation).
// ---------------------------------------------------------------------------
bool aiAskOneShot(const String &prompt, String &outReply) {
  if (prompt.length() == 0) return false;
  String body = buildChatBody(devcfgLlmModel(),
                              nullptr, 0,
                              prompt,
                              /*maxTokens=*/ 512,
                              /*temperature=*/ 1.1f);
  return postChatThroughX402(body, outReply);
}
