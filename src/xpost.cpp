#include "xpost.h"
#include "ai.h"
#include "devcfg.h"
#include "voice.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char *X_API_URL = "https://api.x.com/2/tweets";

// Hard cap: X still rejects >280 chars for non-Premium. We compose at 260
// to give the LLM slack, truncate to 278 + ellipsis on overruns.
static constexpr int TWEET_HARD_CAP = 280;

// Keep the last 10 successful posts for the on-device history screen.
static constexpr size_t RECENT_CAP = 10;

// ---------------------------------------------------------------------------
// Ring buffer of recent posts (newest first when read out).
// ---------------------------------------------------------------------------
static XPostRecent s_recent[RECENT_CAP];
static size_t      s_recentN    = 0;   // count of valid entries (<= RECENT_CAP)
static size_t      s_recentHead = 0;   // index of oldest slot to overwrite

static void pushRecent(const String &id, const String &text) {
  s_recent[s_recentHead].tweetId  = id;
  s_recent[s_recentHead].text     = text;
  s_recent[s_recentHead].postedMs = millis();
  s_recentHead = (s_recentHead + 1) % RECENT_CAP;
  if (s_recentN < RECENT_CAP) s_recentN++;
}

size_t xpostGetRecent(XPostRecent *out, size_t cap) {
  // Walk backwards from the most recent write so the caller sees newest
  // first. s_recentHead points at the *next* write slot, so
  // (s_recentHead - 1 - i) mod RECENT_CAP is the i-th newest entry.
  size_t n = min(s_recentN, cap);
  for (size_t i = 0; i < n; ++i) {
    size_t idx = (s_recentHead + RECENT_CAP - 1 - i) % RECENT_CAP;
    out[i] = s_recent[idx];
  }
  return n;
}

size_t xpostRecentCount() { return s_recentN; }

// ---------------------------------------------------------------------------
// Scheduler state + last-error cache.
// ---------------------------------------------------------------------------
static uint32_t s_lastPostMs     = 0;
static uint32_t s_backoffMs      = 0;   // extra delay after a failure
static String   s_lastError;
static bool     s_firstTickDone  = false;

uint32_t xpostLastSuccessMs() { return s_lastPostMs; }
String   xpostLastError()     { return s_lastError; }

// ---------------------------------------------------------------------------
// OAuth 1.0a helpers
// ---------------------------------------------------------------------------
// RFC 3986 percent-encoding — stricter than `URLEncode`. Only ALPHA, DIGIT
// and `-._~` pass through untouched; everything else (including spaces) is
// %XX encoded with upper-case hex. This matters because OAuth signature
// bases must be byte-identical across client + server.
static String pctEncode(const String &in) {
  String out;
  out.reserve(in.length() * 2);
  for (size_t i = 0; i < in.length(); ++i) {
    uint8_t c = (uint8_t)in[i];
    bool keep =
        (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~';
    if (keep) {
      out += (char)c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

static String randomNonceHex(int bytes) {
  String out;
  for (int i = 0; i < bytes; ++i) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", (unsigned)esp_random() & 0xFFu);
    out += buf;
  }
  return out;
}

// HMAC-SHA1 via mbedtls (already linked in by the ESP32 arduino core). The
// output is 20 bytes; caller passes a buffer of that size.
static bool hmacSha1(const String &key, const String &msg, uint8_t out[20]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (!info) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, /*hmac=*/1) != 0) { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return true;
}

static String base64Encode(const uint8_t *data, size_t len) {
  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
  std::vector<uint8_t> buf(outLen + 1, 0);
  size_t written = 0;
  mbedtls_base64_encode(buf.data(), buf.size(), &written, data, len);
  return String((const char*)buf.data());
}

// Build the full `Authorization: OAuth …` header value for a POST to
// `url` with JSON body (body is NOT included in the signature base per
// RFC 5849 §3.4.1.3.1 — only form-urlencoded bodies participate).
static String buildOAuthHeader(const String &method,
                               const String &url,
                               const String &consumerKey,
                               const String &consumerSecret,
                               const String &accessToken,
                               const String &accessTokenSecret) {
  // 1. Generate per-request nonce + timestamp.
  String nonce = randomNonceHex(16);           // 32 hex chars
  time_t now = time(nullptr);
  char tsBuf[16];
  snprintf(tsBuf, sizeof(tsBuf), "%ld", (long)now);
  String ts = String(tsBuf);

  // 2. Collect the six mandatory oauth_* parameters. Ordering-insensitive
  //    here because the signature step sorts them alphabetically.
  struct Pair { String k; String v; };
  Pair params[] = {
    {"oauth_consumer_key",     consumerKey},
    {"oauth_nonce",            nonce},
    {"oauth_signature_method", "HMAC-SHA1"},
    {"oauth_timestamp",        ts},
    {"oauth_token",            accessToken},
    {"oauth_version",          "1.0"},
  };
  constexpr int N = sizeof(params) / sizeof(params[0]);

  // 3. Sort params alphabetically by encoded key. Simple bubble sort —
  //    N=6 so perf doesn't matter.
  for (int i = 0; i < N - 1; ++i) {
    for (int j = 0; j < N - 1 - i; ++j) {
      if (params[j].k > params[j+1].k) {
        Pair t = params[j]; params[j] = params[j+1]; params[j+1] = t;
      }
    }
  }

  // 4. Build the signature base string: "POST&pct(url)&pct(joined params)"
  String joined;
  for (int i = 0; i < N; ++i) {
    if (i > 0) joined += '&';
    joined += pctEncode(params[i].k);
    joined += '=';
    joined += pctEncode(params[i].v);
  }
  String base = method + "&" + pctEncode(url) + "&" + pctEncode(joined);

  // 5. Signing key: pct(consumerSecret) + "&" + pct(tokenSecret).
  String signingKey = pctEncode(consumerSecret) + "&" + pctEncode(accessTokenSecret);

  // 6. HMAC-SHA1 + base64.
  uint8_t mac[20];
  if (!hmacSha1(signingKey, base, mac)) return String();
  String signature = base64Encode(mac, sizeof(mac));

  // 7. Build the Authorization header. Values are pct-encoded and wrapped
  //    in double quotes; keys are not encoded (they're all ASCII anyway).
  String h = "OAuth ";
  h += "oauth_consumer_key=\""     + pctEncode(consumerKey)      + "\", ";
  h += "oauth_nonce=\""            + pctEncode(nonce)            + "\", ";
  h += "oauth_signature=\""        + pctEncode(signature)        + "\", ";
  h += "oauth_signature_method=\"HMAC-SHA1\", ";
  h += "oauth_timestamp=\""        + ts                          + "\", ";
  h += "oauth_token=\""            + pctEncode(accessToken)      + "\", ";
  h += "oauth_version=\"1.0\"";
  return h;
}

// ---------------------------------------------------------------------------
// Text sanitisation — tweets are 280 chars, no markdown, no fenced code,
// no 4-byte emoji (same rationale as sanitizeForSpeech in main.cpp).
// ---------------------------------------------------------------------------
static String sanitizeForTweet(const String &in) {
  // Strip fenced code blocks.
  String s = in;
  while (true) {
    int a = s.indexOf("```");
    if (a < 0) break;
    int b = s.indexOf("```", a + 3);
    if (b < 0) { s = s.substring(0, a); break; }
    s = s.substring(0, a) + s.substring(b + 3);
  }
  // Inline markdown.
  s.replace("`", "");
  s.replace("**", "");
  s.replace("__", "");

  // Drop 4-byte UTF-8 (emoji) + control chars; keep BMP text.
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ) {
    uint8_t c = (uint8_t)s[i];
    if (c >= 0xF0) { i += 4; continue; }
    if (c >= 0xE0) { out += s[i]; out += s[i+1]; out += s[i+2]; i += 3; continue; }
    if (c >= 0xC0) { out += s[i]; out += s[i+1]; i += 2; continue; }
    if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) out += (char)c;
    i += 1;
  }

  // Collapse whitespace runs. Also strip wrapping quotes the LLM
  // sometimes adds ("Here's a tweet: ...").
  String collapsed;
  collapsed.reserve(out.length());
  bool lastWs = false;
  for (size_t i = 0; i < out.length(); ++i) {
    char c = out[i];
    bool isWs = (c == ' ' || c == '\n' || c == '\t' || c == '\r');
    if (isWs) {
      if (!lastWs) collapsed += ' ';
      lastWs = true;
    } else {
      collapsed += c;
      lastWs = false;
    }
  }
  collapsed.trim();
  if (collapsed.startsWith("\"") && collapsed.endsWith("\"") &&
      collapsed.length() >= 2) {
    collapsed = collapsed.substring(1, collapsed.length() - 1);
    collapsed.trim();
  }

  // Final length cap with a clean word boundary.
  if (collapsed.length() > TWEET_HARD_CAP) {
    int cut = TWEET_HARD_CAP - 1;
    int space = collapsed.lastIndexOf(' ', cut);
    if (space > TWEET_HARD_CAP / 2) cut = space;
    collapsed = collapsed.substring(0, cut) + "…";
  }
  return collapsed;
}

// ---------------------------------------------------------------------------
// Public: submit a single tweet.
// ---------------------------------------------------------------------------
XPostResult xpostSubmit(const String &text) {
  XPostResult r;
  if (WiFi.status() != WL_CONNECTED) {
    r.error = "wifi offline";
    return r;
  }
  if (!devcfgXCredentialsPresent()) {
    r.error = "missing API credentials";
    return r;
  }
  // Clock check — if time() still looks like the epoch we'll send a
  // timestamp 50+ years off and OAuth will reject us with 401.
  time_t now = time(nullptr);
  if (now < 1700000000) {
    r.error = "clock not synced (NTP pending)";
    return r;
  }
  String cleaned = text;
  cleaned.trim();
  if (cleaned.length() == 0) {
    r.error = "empty tweet";
    return r;
  }
  if (cleaned.length() > TWEET_HARD_CAP) {
    // Caller should already sanitize, but guard one last time.
    cleaned = cleaned.substring(0, TWEET_HARD_CAP - 1) + "…";
  }

  String auth = buildOAuthHeader("POST", X_API_URL,
                                 devcfgXApiKey(),
                                 devcfgXApiSecret(),
                                 devcfgXAccessToken(),
                                 devcfgXAccessTokenSecret());
  if (auth.length() == 0) {
    r.error = "signature failed";
    return r;
  }

  // Build the JSON body. We only set `text` — X accepts other fields
  // (media_ids, reply.in_reply_to_tweet_id, …) but we don't expose those.
  String escaped = cleaned;
  escaped.replace("\\", "\\\\");
  escaped.replace("\"", "\\\"");
  escaped.replace("\n", "\\n");
  escaped.replace("\r", "\\r");
  escaped.replace("\t", "\\t");
  String body = String("{\"text\":\"") + escaped + "\"}";

  WiFiClientSecure client;
  client.setInsecure();                     // same policy as x402.cpp
  HTTPClient http;
  if (!http.begin(client, X_API_URL)) {
    r.error = "http.begin failed";
    return r;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", auth);
  http.setTimeout(15000);

  int code = http.POST(body);
  r.httpStatus = code;
  String resp = http.getString();
  http.end();

  if (code == 201 || code == 200) {
    // Success — body is { "data": { "id": "...", "text": "..." } }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp);
    if (!err && doc["data"]["id"].is<const char*>()) {
      r.tweetId = doc["data"]["id"].as<String>();
    }
    r.ok = true;
    pushRecent(r.tweetId, cleaned);
    Serial.printf("xpost: OK id=%s text=\"%s\"\n",
                  r.tweetId.c_str(), cleaned.c_str());
    return r;
  }

  // Error path — surface X's detail field if present, else a short
  // HTTP-status description.
  {
    JsonDocument doc;
    if (deserializeJson(doc, resp) == DeserializationError::Ok) {
      const char *detail = doc["detail"] | doc["title"] | (const char*)nullptr;
      if (detail && *detail) {
        r.error = String(code) + ": " + detail;
      }
    }
  }
  if (r.error.length() == 0) {
    r.error = "http " + String(code);
  }
  Serial.printf("xpost: FAIL http=%d err=%s body=%s\n",
                code, r.error.c_str(), resp.c_str());
  return r;
}

// ---------------------------------------------------------------------------
// Compose-and-post helpers
// ---------------------------------------------------------------------------
static XPostResult composeAndPost() {
  XPostResult fail;
  String topic = devcfgXPostPrompt();
  topic.trim();
  if (topic.length() == 0) {
    fail.error = "prompt empty — set one in the web portal";
    s_lastError = fail.error;
    return fail;
  }

  // Keep the composition prompt tight so the LLM doesn't pad with
  // "Here's a tweet:" or markdown. sanitizeForTweet() catches stragglers.
  String composePrompt =
      "Write one tweet about this topic: " + topic + ". "
      "Rules: plain text only, at most 260 characters, no hashtags unless "
      "they flow naturally, no emoji, no leading 'Tweet:' or quote marks, "
      "no markdown. Output only the tweet itself, nothing else.";

  String raw;
  if (!aiAskOneShot(composePrompt, raw)) {
    fail.error = "LLM compose failed";
    s_lastError = fail.error;
    return fail;
  }

  String cleaned = sanitizeForTweet(raw);
  if (cleaned.length() == 0) {
    fail.error = "LLM returned empty text";
    s_lastError = fail.error;
    return fail;
  }

  XPostResult r = xpostSubmit(cleaned);
  if (r.ok) {
    s_lastError = "";
    s_lastPostMs = millis();
    s_backoffMs  = 0;
  } else {
    s_lastError = r.error;
    // Exponential-ish backoff after failure: double up to 24 h so a bad
    // credential doesn't hammer X once per interval.
    if (s_backoffMs == 0)         s_backoffMs = 5UL * 60UL * 1000UL;   // 5 min
    else if (s_backoffMs < 60UL * 60UL * 1000UL) s_backoffMs *= 2;
  }
  return r;
}

XPostResult xpostRunNow() {
  return composeAndPost();
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------
void xpostSchedulerTick() {
  if (!devcfgXPostEnabled())          return;
  if (!devcfgXCredentialsPresent())   return;
  if (WiFi.status() != WL_CONNECTED)  return;
  if (voiceIsSpeaking())              return;   // share rule w/ heartbeat

  uint32_t intervalMs = devcfgXPostIntervalMin() * 60000UL;
  if (intervalMs < 15UL * 60000UL) intervalMs = 15UL * 60000UL;

  // First run after boot waits one full interval so we don't spam on
  // every reset. Subsequent runs fire `interval + backoff` after the
  // last successful post.
  if (!s_firstTickDone) {
    s_lastPostMs = millis();
    s_firstTickDone = true;
    return;
  }
  uint32_t due = s_lastPostMs + intervalMs + s_backoffMs;
  if ((int32_t)(millis() - due) < 0) return;

  Serial.println("xpost: scheduled run firing");
  composeAndPost();
  // Whether success or failure, push the next attempt to `now + interval`
  // via s_lastPostMs (success) or `now + interval + backoff` (failure,
  // s_backoffMs already bumped inside composeAndPost).
  if (s_lastError.length() != 0) {
    s_lastPostMs = millis();
  }
}