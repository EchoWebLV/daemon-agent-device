#include "ai.h"
#include "secrets.h"
#include "wallet.h"
#include "price.h"
#include "devcfg.h"
#include "x402.h"
#include "tools.h"
#include "memory.h"
#include "mood.h"

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

  // Inner state + recent-self anti-repeat block. This is the single
  // biggest lever against "Daemon always says the same thing" — the
  // system prompt is now materially different every turn based on
  // uptime, local hour, silence length, mood/energy/curiosity, and
  // the last few things Daemon already said (with an explicit "don't
  // reuse these phrases" instruction). See src/mood.cpp for the
  // update rules.
  out += moodPromptContext();

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

static constexpr int MAX_TURNS = 6;
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
// sol.blockrun.ai OpenAI-compatible gateway (x402 USDC paywall on Solana).
// Every chat round — including each tool follow-up — is a paid call.
// ---------------------------------------------------------------------------
static const char *LLM_ENDPOINT        = "https://sol.blockrun.ai/api/v1/chat/completions";
static constexpr int MAX_TOOL_ROUNDS   = 4;   // cap runaway loops

// Seed the working-messages array with system + persisted history.
static void seedWorkingMessages(JsonArray msgs) {
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"]    = "system";
  sys["content"] = buildSystemPrompt();
  for (int i = 0; i < s_histLen; ++i) {
    JsonObject m = msgs.add<JsonObject>();
    m["role"]    = (s_history[i].role == "model") ? "assistant" : "user";
    m["content"] = s_history[i].text;
  }
}

// Perform one round of chat completion through the x402 gateway. On 200,
// parses the message into `outMessage` (the full `choices[0].message`
// object, tool_calls included if present) and the finish reason into
// `outFinish`. Returns false on failure; `outReply` gets a human error.
static bool chatRoundThroughX402(const String &model,
                                 JsonArrayConst workingMessages,
                                 JsonArrayConst extraTools,
                                 int maxTokens,
                                 float temperature,
                                 JsonDocument &outResp,
                                 String &outReply) {
  JsonDocument body;
  body["model"]       = model;
  body["max_tokens"]  = maxTokens;
  body["temperature"] = temperature;
  JsonArray msgs = body["messages"].to<JsonArray>();
  for (JsonVariantConst v : workingMessages) msgs.add(v);
  if (!extraTools.isNull() && extraTools.size() > 0) {
    JsonArray t = body["tools"].to<JsonArray>();
    for (JsonVariantConst v : extraTools) t.add(v);
    body["tool_choice"] = "auto";
  }
  String payload;
  serializeJson(body, payload);

  X402Result r = x402Post(String(LLM_ENDPOINT), payload);
  if (r.status != 200) {
    Serial.printf("ai: x402 %d (err=%s)\n", r.status, r.error.c_str());
    outReply = (r.status == 402)
             ? String("x402 payment failed: ") + r.error
             : (r.error.length() > 0 ? r.error : String("HTTP ") + r.status);
    return false;
  }
  if (r.costUsd > 0) {
    Serial.printf("ai: paid $%.5f USDC (this LLM round)\n", r.costUsd);
  }

  DeserializationError e = deserializeJson(outResp, r.body,
                              DeserializationOption::NestingLimit(32));
  if (e) {
    Serial.printf("ai: parse err=%s\n", e.c_str());
    outReply = "The reply was scrambled.";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Shared tool-calling driver. Runs up to MAX_TOOL_ROUNDS of
//    LLM → (tool_calls → x402 service → tool result) → LLM …
// and returns the final assistant text.
// ---------------------------------------------------------------------------
static bool runChatLoop(JsonArray workingMsgs,
                        int maxTokens,
                        float temperature,
                        String &outReply) {
  // Build tool definitions once up front — the list of enabled services
  // doesn't change mid-conversation.
  JsonDocument toolsDoc;
  toolsDoc.to<JsonArray>();
  toolsBuildArray(toolsDoc.as<JsonArray>());

  for (int round = 0; round < MAX_TOOL_ROUNDS; ++round) {
    JsonDocument resp;
    if (!chatRoundThroughX402(devcfgLlmModel(),
                              workingMsgs,
                              toolsDoc.as<JsonArrayConst>(),
                              maxTokens, temperature,
                              resp, outReply)) {
      return false;
    }

    JsonObjectConst msg   = resp["choices"][0]["message"].as<JsonObjectConst>();
    const char *finish    = resp["choices"][0]["finish_reason"] | "";
    JsonArrayConst calls  = msg["tool_calls"].as<JsonArrayConst>();

    if (!calls.isNull() && calls.size() > 0) {
      Serial.printf("ai: tool round %d — %d call(s)\n", round + 1, calls.size());

      // 1) Echo the assistant's tool-call turn into working messages so
      //    the LLM has context for the subsequent tool results.
      JsonObject asst = workingMsgs.add<JsonObject>();
      asst["role"] = "assistant";
      const char *content = msg["content"] | (const char*)nullptr;
      if (content) asst["content"] = content;
      else         asst["content"] = nullptr;
      JsonArray asstCalls = asst["tool_calls"].to<JsonArray>();
      for (JsonVariantConst c : calls) asstCalls.add(c);

      // 2) Execute each tool and append its result as a role=tool message.
      for (JsonObjectConst c : calls) {
        const char *callId   = c["id"]                       | "";
        const char *fnName   = c["function"]["name"]         | "";
        const char *fnArgs   = c["function"]["arguments"]    | "";
        Serial.printf("ai: → %s(%s)\n", fnName, fnArgs);
        String result = toolExecute(fnName, fnArgs);
        Serial.printf("ai: ← %.120s%s\n",
                      result.c_str(), result.length() > 120 ? "…" : "");
        JsonObject toolMsg = workingMsgs.add<JsonObject>();
        toolMsg["role"]         = "tool";
        toolMsg["tool_call_id"] = callId;
        toolMsg["content"]      = result;
      }
      continue;   // loop back, the LLM gets another round
    }

    // No more tool calls — we have the final answer.
    const char *txt = msg["content"] | "";
    String reply = String(txt);
    reply.trim();
    if (reply.length() == 0) {
      Serial.printf("ai: empty reply, finish=%s\n", finish);
      outReply = "I forgot what I was going to say.";
      return false;
    }
    outReply = reply;
    return true;
  }

  outReply = "Too many tool rounds — bailing.";
  return false;
}

// ---------------------------------------------------------------------------
// Multi-turn chat with persistent history. Each call flows through the
// tool-calling loop above.
// ---------------------------------------------------------------------------
bool aiAsk(const String &userText, String &outReply) {
  if (userText.length() == 0) return false;
  pushTurn("user", userText);

  JsonDocument msgsDoc;
  JsonArray working = msgsDoc.to<JsonArray>();
  seedWorkingMessages(working);

  // Temperature + token budget are mood-driven now: a tired / grumpy
  // Daemon gets terser and more predictable, a curious / wired Daemon
  // gets hotter and more associative. That variance is also a big part
  // of why replies stop feeling samey.
  const float temp    = moodTemperature();
  const int   maxTok  = moodMaxTokens();

  String reply;
  bool ok = runChatLoop(working, maxTok, temp, reply);
  if (!ok) {
    s_histLen--;              // roll back the pending user turn
    outReply = reply;
    return false;
  }
  pushTurn("model", reply);

  // Feed the reply back into the anti-repeat ring so the next prompt's
  // "do NOT reuse these phrases" block actually knows what we just said.
  moodNoteDaemonReply(reply);

  // Persist this exchange to on-chain memory (fire-and-forget; runs on a
  // background task so chat latency stays the same).
  memoryRecordExchange(userText, reply);

  outReply = reply;
  return true;
}

void aiLoadHistory(const MemoryTurn *turns, int count) {
  if (!turns || count <= 0) return;
  aiResetHistory();
  // Push in order; `pushTurn` handles the FIFO trimming if we overflow.
  for (int i = 0; i < count; ++i) {
    pushTurn(turns[i].role, turns[i].text);
  }
  Serial.printf("ai: loaded %d turn(s) into history from memory\n", s_histLen);
}

// ---------------------------------------------------------------------------
// One-shot prompt (doesn't touch history).
// ---------------------------------------------------------------------------
bool aiAskOneShot(const String &prompt, String &outReply) {
  if (prompt.length() == 0) return false;

  JsonDocument msgsDoc;
  JsonArray working = msgsDoc.to<JsonArray>();
  JsonObject sys = working.add<JsonObject>();
  sys["role"]    = "system";
  sys["content"] = buildSystemPrompt();
  JsonObject usr = working.add<JsonObject>();
  usr["role"]    = "user";
  usr["content"] = prompt;

  // One-shot prompts (heartbeat chatter, tweet composition) also benefit
  // from mood-driven variance — a sleepy 3am ambient line should sound
  // different from a wired mid-afternoon one.
  const float temp   = moodTemperature();
  const int   maxTok = moodMaxTokens();
  bool ok = runChatLoop(working, maxTok, temp, outReply);
  if (ok) moodNoteDaemonReply(outReply);
  return ok;
}

// ---------------------------------------------------------------------------
// Lightweight one-shot — minimal system prompt to save heap.
// The full buildSystemPrompt() includes wallet context, mood state, and
// anti-repeat history which can add ~2 KB of text. On the Moltbook worker
// task, that extra payload pushes heap below the NetGate DRAM floor after
// the first 402 response, making the RPC calls for the payment impossible.
// This variant uses only the personality string.
// ---------------------------------------------------------------------------
bool aiAskOneShotLite(const String &prompt, String &outReply) {
  if (prompt.length() == 0) return false;

  String personality = devcfgPersonality();
  if (personality.length() == 0) personality = String(PERSONA);

  JsonDocument msgsDoc;
  JsonArray working = msgsDoc.to<JsonArray>();
  JsonObject sys = working.add<JsonObject>();
  sys["role"]    = "system";
  sys["content"] = personality;
  JsonObject usr = working.add<JsonObject>();
  usr["role"]    = "user";
  usr["content"] = prompt;

  // Single round, no tools — saves heap by skipping tool definitions and
  // the tool-calling loop. Critical for the Moltbook worker where every
  // KB counts to stay above the NetGate DRAM floor.
  JsonDocument resp;
  JsonArrayConst noTools;   // null → no tools sent
  if (!chatRoundThroughX402(devcfgLlmModel(), working, noTools,
                            256, 0.9f, resp, outReply)) {
    return false;
  }
  const char *txt = resp["choices"][0]["message"]["content"] | "";
  String reply = String(txt);
  reply.trim();
  if (reply.length() == 0) {
    outReply = "I forgot what I was going to say.";
    return false;
  }
  outReply = reply;
  moodNoteDaemonReply(outReply);
  return true;
}
