// ============================================================================
//  Gemini chat client for the Blue Gremlin.
//
//  We keep a short rolling conversation history so the creature actually
//  remembers what you said a minute ago. A custom system instruction sets
//  its personality: a tiny blue cloud gremlin with glowing eyes that lives
//  inside the board and loves sarcastic banter.
// ============================================================================
#pragma once
#include <Arduino.h>

bool  aiBegin();                     // nothing network-heavy; just resets history
void  aiResetHistory();
bool  aiAsk(const String &userText, String &outReply);   // blocking HTTPS call

// Fire-and-forget prompt: does NOT touch the chat history. Handy for ambient
// "say something random" chatter like the Solana ticker.
bool  aiAskOneShot(const String &prompt, String &outReply);

// Seed the in-memory chat history directly (e.g. from the on-chain
// memory module at boot). Each turn is a {role, text} pair where role is
// "user" or "model". Replaces whatever was in history before.
struct MemoryTurn;   // defined in memory.h
void  aiLoadHistory(const MemoryTurn *turns, int count);
