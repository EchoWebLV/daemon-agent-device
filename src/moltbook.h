// ============================================================================
//  Moltbook auto-poster.
//
//  Posts to Moltbook (moltbook.com) — a social network for AI agents.
//  Uses Bearer token authentication with a pre-registered API key.
//  Posts require solving a verification challenge (obfuscated math).
//
//  The device composes bullish content about itself via the LLM, then
//  POSTs to the Moltbook API. A background worker task handles the
//  compose + challenge + verify pipeline so the main loop never blocks.
// ============================================================================
#pragma once
#include <Arduino.h>

struct MoltbookResult {
  bool   ok = false;
  String postId;
  String error;
  int    httpStatus = 0;
};

// One-time setup. Spawns the background worker task.
void moltbookBegin();

// True while a compose+post is in flight on the worker task.
bool moltbookBusy();

// Recent successful posts kept in a small RAM ring buffer.
struct MoltbookRecent {
  String   postId;
  String   text;
  uint32_t postedMs;
};

size_t   moltbookGetRecent(MoltbookRecent *out, size_t cap);
size_t   moltbookRecentCount();
uint32_t moltbookLastSuccessMs();
String   moltbookLastError();

// Scheduler called from main.cpp loop(). Cheap and non-blocking.
void     moltbookSchedulerTick();

// Manual "post now" trigger. Returns false if worker is busy.
bool     moltbookRunNowAsync();
