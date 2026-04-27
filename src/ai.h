// ---------------------------------------------------------------------------
//  Chat client for the Daemon creature.
//
//  Wraps a short rolling conversation history around calls to our own
//  daemon-x402s facilitator (Next.js on Vercel). The facilitator gates
//  /api/openai and /api/anthropic behind an x402 USDC paywall, so every
//  reply the creature utters is a USDC-charged round-trip through
//  x402_post(), which signs and submits the payment transaction on the fly.
//
//  System prompt is rebuilt on every request so fresh wallet balances +
//  SOL price show up in the context the model sees.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clear the in-memory chat history. Safe to call more than once.
bool ai_begin(void);
void ai_reset_history(void);

// Multi-turn chat. Appends the user turn to history, POSTs to the LLM,
// parses out the assistant reply, and appends that too. On failure,
// `reply_out` is filled with a short human-readable message; history
// is rolled back so the next attempt starts from a clean slate.
bool ai_ask(const char *user, char *reply_out, size_t reply_cap);

// Single-turn prompt that does NOT touch the conversation history. Used
// for ambient / one-off utterances (e.g. the wallet ticker chatter).
bool ai_ask_one_shot(const char *prompt, char *reply_out, size_t reply_cap);

// Server callback signature. Delegates to ai_ask(). Registered via
// server_set_say_handler() from app_main.
void ai_handle_say(const char *user, char *reply_out, size_t reply_cap);

#ifdef __cplusplus
}
#endif
