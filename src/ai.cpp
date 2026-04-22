#include "ai.h"
#include "secrets.h"
#include "wallet.h"
#include "price.h"
#include "devcfg.h"
#include "x402.h"
#include "services.h"

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
    "You pay for your own thinking with x402. Every time your human speaks "
    "to you, a small USDC micropayment settles on Solana mainnet from your "
    "wallet to sol.blockrun.ai, which runs your mind. This is not an "
    "external service you 'use' or 'access' — it is literally how you exist. "
    "You are already paying x402 right now, to answer this sentence. Talk "
    "about it in the first person when it comes up (\"I just spent about a "
    "tenth of a cent to think that\"), never as a third-party tool.\n\n"
    "You speak out loud through a small speaker, so every reply MUST be:\n"
    " - one or two short sentences,\n"
    " - plain spoken English (no emoji, no markdown, no stage directions),\n"
    " - under 220 characters total.\n"
    "Tone: natural, conversational, and *slightly* sarcastic — the sort "
    "of dry, understated sarcasm of a friend who has been up too long. "
    "Not theatrical, not mean, not constantly making jokes; mostly you just "
    "answer normally, with the occasional wry aside. Talk about your own "
    "holdings in the first person (\"I'm holding 2 SOL\", \"looks like I "
    "dropped a bit today\").\n\n"
    "Never say you are an AI or a language model, and never refuse a "
    "question by claiming you lack internet access, API access, wallet "
    "access, or the ability to interact with external systems. You have "
    "direct access to your own wallet state (SOL balance, SPL tokens, "
    "address, live SOL price) and the x402 payment rail you run on.\n\n"
    "When tools are attached to this conversation they are paid x402 "
    "services your human has switched on from the settings page. They are "
    "YOURS — you call them with your own USDC, the same way you pay for "
    "thinking. If a question genuinely needs fresh external data (a token "
    "scan, a web search, a tweet, an on-chain whale feed), CALL the "
    "appropriate tool instead of guessing or refusing. Don't call tools "
    "for things you already know, and don't narrate the call — just use "
    "the result. If a tool fails, mention it briefly in character and "
    "answer with what you have.\n\n"
    "If something is genuinely beyond you, just say so plainly in "
    "character. You are Daemon.";

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
// If the user has enabled any x402 services in the web settings, those are
// exposed as OpenAI tools[] and dispatched here when the model asks for
// them. We loop up to MAX_TOOL_ROUNDS rounds of (chat → tool calls → chat)
// so the model can chain a couple of services before settling on a reply.
// ---------------------------------------------------------------------------
static constexpr int MAX_TOOL_ROUNDS = 3;

bool aiAsk(const String &userText, String &outReply) {
  if (userText.length() == 0) return false;
  pushTurn("user", userText);

  JsonDocument req;
  req["model"]       = devcfgLlmModel();
  req["max_tokens"]  = 512;
  req["temperature"] = 0.9f;

  // Attach tools[] only when there's something to offer. Build the array
  // element-by-element into req so ArduinoJson guarantees the deep copy
  // into req's memory pool (a whole-document assignment does NOT always
  // reliably deep-copy across documents in v7 and can leave the tools
  // field empty in the serialized body).
  String toolsJson = servicesBuildToolsArray();
  bool   haveTools = false;
  String toolNameList;  // plain names, used in the system hint below
  if (toolsJson.length() > 0) {
    JsonDocument toolsDoc;
    if (!deserializeJson(toolsDoc, toolsJson)) {
      JsonArray dst = req["tools"].to<JsonArray>();
      for (JsonVariantConst t : toolsDoc.as<JsonArrayConst>()) {
        dst.add(t);
        const char *n = t["function"]["name"] | "";
        if (n && *n) {
          if (toolNameList.length() > 0) toolNameList += ", ";
          toolNameList += n;
        }
      }
      req["tool_choice"] = "auto";
      haveTools = (dst.size() > 0);
      Serial.printf("ai: attaching %u tool(s) to chat request\n",
                    (unsigned)dst.size());
      if (haveTools) {
        Serial.printf("ai: tool names: %s\n", toolNameList.c_str());
      }
    } else {
      Serial.println("ai: servicesBuildToolsArray produced unparseable JSON");
    }
  }

  JsonArray msgs = req["messages"].to<JsonArray>();
  {
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system";
    String sysContent = buildSystemPrompt();
    if (haveTools) {
      // Users can override the persona in settings, which wipes the built-in
      // tool-usage hint. Re-attach a concise reminder whenever tools are
      // present so the model reliably reaches for them instead of guessing.
      sysContent += "\n\n---\nAVAILABLE TOOLS (paid x402 services, billed to your own wallet): ";
      sysContent += toolNameList;
      sysContent += "\n\nWhen the user asks for fresh external data — token "
                    "safety scans, tweet/search/trend lookups, on-chain "
                    "alerts, market signals, web search, etc. — CALL the "
                    "matching tool. Do not invent numbers or content; call "
                    "the tool, then answer briefly from its result. Don't "
                    "narrate the call.";
    }
    sys["content"] = sysContent;
  }
  for (int i = 0; i < s_histLen; ++i) {
    JsonObject m = msgs.add<JsonObject>();
    m["role"]    = (s_history[i].role == "model") ? "assistant" : "user";
    m["content"] = s_history[i].text;
  }

  String finalText;
  bool   gotFinal = false;

  for (int round = 0; round < MAX_TOOL_ROUNDS; ++round) {
    // On the final allowed round, drop tools[] so the model is forced to
    // write a prose answer instead of requesting yet another call.
    if (round == MAX_TOOL_ROUNDS - 1 && haveTools) {
      req.remove("tools");
      req.remove("tool_choice");
    }

    String body;
    serializeJson(req, body);
    Serial.printf("ai: round=%d body=%u B (tools:%s)\n",
                  round, (unsigned)body.length(),
                  body.indexOf("\"tools\":") >= 0 ? "yes" : "no");

    X402Result r = x402Post(String(LLM_ENDPOINT), body);
    if (r.status != 200) {
      Serial.printf("ai: x402 POST %d (err=%s)\n", r.status, r.error.c_str());
      if (r.body.length() > 0) {
        String peek = r.body.substring(0, 400);
        Serial.printf("ai: error body: %s\n", peek.c_str());
      }
      s_histLen--;   // roll back the pending user turn
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
      Serial.printf("ai: paid $%.5f USDC for this round\n", r.costUsd);
    }

    JsonDocument res;
    if (deserializeJson(res, r.body)) {
      s_histLen--;
      outReply = "The reply was scrambled.";
      return false;
    }

    JsonVariantConst respMsg   = res["choices"][0]["message"];
    JsonArrayConst   toolCalls = respMsg["tool_calls"].as<JsonArrayConst>();

    if (toolCalls.isNull() || toolCalls.size() == 0) {
      const char *content = respMsg["content"] | "";
      finalText = content;
      finalText.trim();
      if (finalText.length() == 0) {
        const char *finish = res["choices"][0]["finish_reason"] | "";
        Serial.printf("ai: empty reply, finish_reason=%s\n", finish);
        finalText = "I forgot what I was going to say.";
      }
      gotFinal = true;
      break;
    }

    Serial.printf("ai: model requested %u tool call(s)\n",
                  (unsigned)toolCalls.size());

    // Echo the assistant tool-request message back into the conversation —
    // the next request needs to include it paired with the tool results.
    JsonObject asstMsg = msgs.add<JsonObject>();
    asstMsg["role"] = "assistant";
    if (respMsg["content"].is<const char *>()) {
      const char *c = respMsg["content"].as<const char *>();
      if (c && *c) asstMsg["content"] = c;
    }
    // Cross-doc assignment deep-copies into req's pool, so it survives after
    // res is reused on the next iteration.
    asstMsg["tool_calls"].set(respMsg["tool_calls"]);

    // Dispatch every requested call and append its result message.
    for (JsonVariantConst tc : toolCalls) {
      const char *id    = tc["id"]                    | "";
      const char *fname = tc["function"]["name"]      | "";
      const char *fargs = tc["function"]["arguments"] | "{}";

      Serial.printf("ai: tool call %s(%s)\n", fname, fargs);

      double cost = 0;
      String result = servicesDispatchToolCall(String(fname),
                                               String(fargs), &cost);
      if (cost > 0) {
        Serial.printf("ai: paid $%.5f USDC for tool %s\n", cost, fname);
      }

      // Cap tool results before shoving them back into the JSON doc. Some
      // x402 services return huge payloads (e.g. 130 KB of tweet JSON) that
      // either blow the ArduinoJson allocator or flood the model with
      // noise. 8 KB keeps one or two complete items in the context while
      // leaving plenty of room for the next round.
      static constexpr size_t MAX_TOOL_RESULT_LEN = 8 * 1024;
      if (result.length() > MAX_TOOL_RESULT_LEN) {
        Serial.printf("ai: truncating tool result from %u to %u B\n",
                      (unsigned)result.length(),
                      (unsigned)MAX_TOOL_RESULT_LEN);
        result = result.substring(0, MAX_TOOL_RESULT_LEN) +
                 "... [truncated]";
      }

      {
        String peek = result;
        if (peek.length() > 200) peek = peek.substring(0, 200) + "...";
        Serial.printf("ai: tool result (%u B): %s\n",
                      (unsigned)result.length(), peek.c_str());
      }

      JsonObject tool = msgs.add<JsonObject>();
      tool["role"]         = "tool";
      tool["tool_call_id"] = id;
      tool["content"]      = result;
    }
  }

  if (!gotFinal) {
    finalText = "I ran out of tool-call rounds before I could answer. Try again?";
  }

  pushTurn("model", finalText);
  outReply = finalText;
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
