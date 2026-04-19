// ============================================================================
//  X (Twitter) auto-poster.
//
//  Posts tweets to the X API v2 (`POST https://api.x.com/2/tweets`) using
//  OAuth 1.0a user-context authentication. The four credential fields
//  (consumer key/secret + access token/secret) live in devcfg and are set
//  via the web portal — the device never talks to X's OAuth web flow.
//
//  The X API v2 `POST /2/tweets` endpoint accepts a JSON body; per RFC 5849
//  the signature base for JSON (non-x-www-form-urlencoded) bodies only
//  includes the oauth_* parameters and URL query string.
//
//  Requires a synchronised wall clock — OAuth 1.0a rejects timestamps more
//  than ~5 minutes off from the X server's clock. main.cpp calls
//  configTime() once Wi-Fi is up; xpostSubmit() refuses to sign if the
//  clock still looks like 1970.
// ============================================================================
#pragma once
#include <Arduino.h>

struct XPostResult {
  bool   ok = false;
  String tweetId;         // populated on success (e.g. "18123456789012345")
  String error;           // short diagnostic when ok == false
  int    httpStatus = 0;  // X API response status (0 = transport failure)
};

// One-time setup. Spawns the background worker task that runs the
// compose + sign + POST pipeline so neither the Arduino loop nor the
// HTTP server task ever blocks on LLM + HTTPS round-trips.
void xpostBegin();

// True while a compose+post is in flight on the worker task.
bool xpostBusy();

// Blocking HTTPS call. Safe to run on any task; not re-entrant (uses a
// single shared WiFiClientSecure + HTTPClient internally).
XPostResult xpostSubmit(const String &text);

// Recent successful posts kept in a small RAM ring buffer so the device
// screen can show a history without hitting the X API (which costs $0.01
// for every read on the current pay-per-use model, and also rate-limits).
struct XPostRecent {
  String   tweetId;
  String   text;
  uint32_t postedMs;   // millis() at post time — relative, not wall clock
};

// Snapshot of the ring buffer (newest first). Returns how many entries
// were written into `out`. Pass a buffer sized to what you want; nothing
// is written past `cap`.
size_t xpostGetRecent(XPostRecent *out, size_t cap);

// Total number of tweets currently held in the ring buffer (<=10).
size_t xpostRecentCount();

// Returns the millis() timestamp of the most recent successful post,
// or 0 if nothing has been posted since boot.
uint32_t xpostLastSuccessMs();

// Returns the last error string (or "" on success / never-run). Updated
// by both scheduled runs and manual "post now" calls.
String   xpostLastError();

// Scheduler called from main.cpp loop(). Cheap and non-blocking:
// enqueues a worker-task run when due. Safe to call every loop iteration.
void     xpostSchedulerTick();

// Manual "post now" trigger — enqueues one run on the worker task and
// returns immediately. Callers watch for a new entry in the recent
// ring buffer (or a bump in xpostLastError) to know the result.
// Returns false if the worker is already busy.
bool     xpostRunNowAsync();