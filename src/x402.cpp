#include "x402.h"
#include "wallet.h"
#include "solana_tx.h"
#include "base58.h"
#include "secrets.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

// ---------------------------------------------------------------------------
// Match the Chrome extension's network id + constants.
// ---------------------------------------------------------------------------
static const char *SOLANA_NETWORK_ID = "solana:5eykt4UsFv8P8NJdTREpY1vzqKqZKvdp";
static const char *USDC_MINT         = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
static constexpr uint8_t USDC_DECIMALS = 6;

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

// When the user has set a Helius key we go straight to it — reliable,
// generous free tier. Without a key we rotate through several public
// Solana RPCs because `api.mainnet-beta.solana.com` alone rate-limits
// aggressively (~40 req/10s shared across IPs), and during a chat burst
// the device easily exceeds that. Each call picks the next provider in
// the list, so even if one is throttled the next attempt hits a fresh
// endpoint.
// Public Solana RPCs that accept unauthenticated POSTs. Verified on-device:
//   - api.mainnet-beta.solana.com: Solana Labs (aggressive rate limit)
//   - solana-rpc.publicnode.com:   Allnodes (generous, reliable)
// Previously tried drpc.org (400s without API key) and ankr.com/solana
// (403s without key) — dropped. Add more here when you confirm they work.
static const char *PUBLIC_RPCS[] = {
  "https://api.mainnet-beta.solana.com",
  "https://solana-rpc.publicnode.com",
};
static constexpr int N_PUBLIC_RPCS = sizeof(PUBLIC_RPCS) / sizeof(PUBLIC_RPCS[0]);

static String pickRpcUrlRotating() {
  String k = String(HELIUS_API_KEY);
  if (!k.startsWith("PASTE-") && k.length() >= 10) {
    return "https://mainnet.helius-rpc.com/?api-key=" + k;
  }
  static uint32_t s_idx = 0;
  const char *ep = PUBLIC_RPCS[s_idx % N_PUBLIC_RPCS];
  s_idx++;
  return String(ep);
}

// rpcCall with retry + backoff. Retries on network error and on 429 /
// 5xx (common rate-limit + transient signals). Each retry picks the next
// endpoint via pickRpcUrlRotating() so a throttled provider doesn't
// pin us down.
static bool rpcCall(const String &payload, String &outBody) {
  if (WiFi.status() != WL_CONNECTED) return false;

  constexpr int MAX_TRIES     = 3;
  constexpr uint32_t BACKOFF  = 350;   // ms between tries (linear)

  for (int attempt = 0; attempt < MAX_TRIES; ++attempt) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = pickRpcUrlRotating();
    if (!http.begin(client, url)) continue;
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);

    int code = http.POST(payload);
    if (code == 200) {
      outBody = http.getString();
      http.end();
      return true;
    }

    // Read and discard body on failure to keep the server state clean
    // and to get a glimpse of the error in logs when it's small.
    String body = http.getString();
    http.end();

    bool retryable = (code == 429) || (code >= 500) || (code < 0);
    Serial.printf("x402: rpc %d on try %d (ep=%.40s) retryable=%d\n",
                  code, attempt + 1, url.c_str(), retryable ? 1 : 0);
    if (!retryable) return false;

    // Linear backoff — keeps things simple; rotating endpoint already
    // gives us most of the benefit of exponential backoff.
    vTaskDelay((BACKOFF * (attempt + 1)) / portTICK_PERIOD_MS);
  }
  return false;
}


// Recent blockhash: returns 32 bytes (base58-decoded) or false.
static bool fetchRecentBlockhash(uint8_t out[32]) {
  String payload = R"({"jsonrpc":"2.0","id":1,"method":"getLatestBlockhash","params":[]})";
  String body;
  if (!rpcCall(payload, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const char *hash = doc["result"]["value"]["blockhash"] | (const char*)nullptr;
  if (!hash) return false;
  auto bytes = base58Decode(String(hash));
  if (bytes.size() != 32) return false;
  memcpy(out, bytes.data(), 32);
  return true;
}

// Small in-memory cache of destination-owner → USDC ATA lookups. Every
// payment to the same recipient would otherwise open a fresh TLS client
// against the Solana RPC mid-x402-flow — three concurrent TLS contexts
// (LLM client + service client + RPC client) has been observed to
// exhaust heap and mystery-reboot the device on busy chats.
struct AtaCacheEntry { String owner; String ata; uint32_t ageMs; };
static constexpr int ATA_CACHE_N = 8;
static AtaCacheEntry s_ataCache[ATA_CACHE_N];

static String lookupAtaCache(const String &ownerB58) {
  for (auto &e : s_ataCache) if (e.owner == ownerB58) return e.ata;
  return String();
}
static void storeAtaCache(const String &ownerB58, const String &ata) {
  int oldestIdx = 0;
  uint32_t oldestAge = 0;
  uint32_t now = millis();
  for (int i = 0; i < ATA_CACHE_N; ++i) {
    if (s_ataCache[i].owner.length() == 0) { oldestIdx = i; break; }
    uint32_t age = now - s_ataCache[i].ageMs;
    if (age > oldestAge) { oldestAge = age; oldestIdx = i; }
  }
  s_ataCache[oldestIdx] = { ownerB58, ata, now };
}

// Find the destination's USDC associated token account via RPC.
// getTokenAccountsByOwner with mint filter returns the ATA if one exists.
// Returns empty String if the recipient has no USDC account set up —
// which means an x402 service is mis-configured, the payment will fail.
static String fetchUsdcAta(const String &ownerB58) {
  String cached = lookupAtaCache(ownerB58);
  if (cached.length() > 0) return cached;

  String payload = String(
      "{\"jsonrpc\":\"2.0\",\"id\":1,"
      "\"method\":\"getTokenAccountsByOwner\",\"params\":[\"") +
      ownerB58 + "\",{\"mint\":\"" + USDC_MINT + "\"},"
      "{\"encoding\":\"jsonParsed\"}]}";
  String body;
  if (!rpcCall(payload, body)) return String();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return String();
  JsonArray arr = doc["result"]["value"].as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) return String();
  const char *pk = arr[0]["pubkey"] | (const char*)nullptr;
  String ata = pk ? String(pk) : String();
  if (ata.length() > 0) storeAtaCache(ownerB58, ata);
  return ata;
}

// Base64-encode a UTF-8 string without newlines.
static String b64EncodeString(const String &s) {
  size_t olen = 0;
  mbedtls_base64_encode(nullptr, 0, &olen,
                        (const uint8_t *)s.c_str(), s.length());
  std::vector<uint8_t> buf(olen + 1, 0);
  size_t written = 0;
  mbedtls_base64_encode(buf.data(), buf.size(), &written,
                        (const uint8_t *)s.c_str(), s.length());
  return String((const char *)buf.data());
}

// ---------------------------------------------------------------------------
// Given a decoded payment-required JSON, build the PAYMENT-SIGNATURE header
// value by signing a Solana USDC transfer.
// ---------------------------------------------------------------------------
static String buildPaymentPayload(const JsonDocument &paymentRequired,
                                  const String       &resourceUrl,
                                  String             *outAmountAtomic) {
  // Pick the Solana accept option (fallback to the first if not found).
  JsonArrayConst accepts = paymentRequired["accepts"].as<JsonArrayConst>();
  if (accepts.isNull() || accepts.size() == 0) return String();
  JsonObjectConst opt = accepts[0];
  for (JsonObjectConst a : accepts) {
    const char *net = a["network"] | "";
    if (strcmp(net, SOLANA_NETWORK_ID) == 0) { opt = a; break; }
  }

  const char *payTo      = opt["payTo"]            | "";
  const char *amountStr  = opt["amount"]           | opt["maxAmountRequired"] | "";
  const char *feePayerS  = opt["extra"]["feePayer"]| "";
  uint16_t    timeoutSec = opt["maxTimeoutSeconds"]| 300;
  if (!*payTo || !*amountStr || !*feePayerS) {
    Serial.println("x402: missing payTo / amount / feePayer");
    return String();
  }

  uint64_t amountAtomic = strtoull(amountStr, nullptr, 10);
  if (outAmountAtomic) *outAmountAtomic = String(amountStr);

  // Source ATA: already cached from our wallet refresh.
  String sourceAtaB58 = walletUsdcAta();
  if (sourceAtaB58.length() == 0) {
    Serial.println("x402: wallet has no USDC account (zero balance? never funded?)");
    return String();
  }

  // Destination ATA: derive via RPC (avoids shipping a PDA + on-curve
  // check implementation on the device).
  String destAtaB58 = fetchUsdcAta(String(payTo));
  if (destAtaB58.length() == 0) {
    Serial.printf("x402: destination %s has no USDC ATA\n", payTo);
    return String();
  }

  // Decode all base58 inputs into raw 32-byte buffers.
  uint8_t feePayerKey[32], sourceAta[32], destAta[32], mintKey[32], blockhash[32];
  auto dec = [](const String &b58, uint8_t out[32]) -> bool {
    auto v = base58Decode(b58);
    if (v.size() != 32) return false;
    memcpy(out, v.data(), 32);
    return true;
  };
  if (!dec(String(feePayerS), feePayerKey)) return String();
  if (!dec(sourceAtaB58,      sourceAta))   return String();
  if (!dec(destAtaB58,        destAta))     return String();
  if (!dec(String(USDC_MINT), mintKey))     return String();
  if (!fetchRecentBlockhash(blockhash)) {
    Serial.println("x402: failed to fetch recent blockhash");
    return String();
  }

  SolanaTxInput in;
  in.feePayer     = feePayerKey;
  in.walletOwner  = walletPubkeyBytes();
  in.sourceAta    = sourceAta;
  in.destAta      = destAta;
  in.mint         = mintKey;
  in.blockhash    = blockhash;
  in.amountAtomic = amountAtomic;
  in.mintDecimals = USDC_DECIMALS;
  in.cuLimit      = 8000;
  in.cuPriceMicro = 1;

  String txBase64 = solanaBuildSignedTxBase64(in);
  if (txBase64.length() == 0) return String();

  // Build the x402 payment envelope JSON (same shape as the Chrome ext).
  String extraJson = "{}";
  if (opt["extra"].is<JsonObjectConst>()) {
    serializeJson(opt["extra"], extraJson);
  }
  String extensionsJson = "{}";
  if (paymentRequired["extensions"].is<JsonObjectConst>()) {
    serializeJson(paymentRequired["extensions"], extensionsJson);
  }

  String envelope;
  envelope.reserve(txBase64.length() + 512);
  envelope  = "{\"x402Version\":2,";
  envelope += "\"resource\":{\"url\":\"";   envelope += resourceUrl;
  envelope += "\",\"description\":\"Daemon x402\",";
  envelope += "\"mimeType\":\"application/json\"},";
  envelope += "\"accepted\":{";
  envelope += "\"scheme\":\"exact\",";
  envelope += "\"network\":\""; envelope += SOLANA_NETWORK_ID; envelope += "\",";
  envelope += "\"amount\":\"";  envelope += amountStr;         envelope += "\",";
  envelope += "\"asset\":\"";   envelope += USDC_MINT;         envelope += "\",";
  envelope += "\"payTo\":\"";   envelope += payTo;             envelope += "\",";
  envelope += "\"maxTimeoutSeconds\":"; envelope += String(timeoutSec); envelope += ",";
  envelope += "\"extra\":"; envelope += extraJson;
  envelope += "},";
  envelope += "\"payload\":{\"transaction\":\""; envelope += txBase64; envelope += "\"},";
  envelope += "\"extensions\":"; envelope += extensionsJson;
  envelope += "}";

  return b64EncodeString(envelope);
}

// ---------------------------------------------------------------------------
// Public API (shared GET/POST handler)
// ---------------------------------------------------------------------------
// Run one HTTPS request to `url` and return (status, body, payment-required
// descriptor). Owns its own TLS client + HTTPClient in a tight scope so
// both are torn down before the next sub-request.
static void doOneRequest(const char *method,
                         const String &url,
                         const String &jsonBody,
                         const String &authBearer,
                         const String &paymentSigHeader,
                         int    &outStatus,
                         String &outBody,
                         String &outPayReq) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(60000);
  if (!http.begin(client, url)) {
    outStatus = -1;
    outBody   = String();
    return;
  }
  if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
  }
  if (authBearer.length() > 0) {
    http.addHeader("Authorization", "Bearer " + authBearer);
  }
  if (paymentSigHeader.length() > 0) {
    http.addHeader("PAYMENT-SIGNATURE", paymentSigHeader);
  }

  static const char *kPaymentHeaders[] = {
    "payment-required",
    "x-payment-required",
    "www-authenticate",
  };
  http.collectHeaders(kPaymentHeaders, 3);

  uint32_t t0 = millis();
  int code = (strcmp(method, "POST") == 0)
             ? http.POST(jsonBody)
             : http.GET();
  Serial.printf("x402: %s %s -> %d in %lu ms (heap=%u)\n",
                method, url.c_str(), code,
                (unsigned long)(millis() - t0),
                (unsigned)ESP.getFreeHeap());

  // Pull the body first.
  outStatus = code;
  outBody   = http.getString();

  // If this is a 402, also extract the payment-required descriptor from
  // whichever of the known headers is present.
  if (code == 402) {
    String rawB64;
    auto tryDecode = [&](const String &b64) {
      if (b64.length() == 0 || outPayReq.length() > 0) return;
      size_t olen = 0;
      std::vector<uint8_t> dec(b64.length() + 4, 0);
      if (mbedtls_base64_decode(dec.data(), dec.size(), &olen,
            (const uint8_t *)b64.c_str(), b64.length()) == 0 && olen > 0) {
        outPayReq = String((const char *)dec.data()).substring(0, olen);
      }
    };
    tryDecode(http.header("payment-required"));
    tryDecode(http.header("x-payment-required"));
    String wa = http.header("www-authenticate");
    int q1 = wa.indexOf('"');
    int q2 = wa.lastIndexOf('"');
    if (q1 >= 0 && q2 > q1) tryDecode(wa.substring(q1 + 1, q2));
    if (outPayReq.length() == 0) outPayReq = outBody;   // fallback: body IS the JSON
  }

  http.end();
  // `client` destructor here frees its mbedTLS context before we return.
}

// Wrap doOneRequest with transient-failure retries. HTTPClient returns
// small negative codes when the TCP connect / TLS handshake fails
// (classically -1 = connection refused, -11 = read timeout). These hit
// in bursts on shared Wi-Fi / hotspot networks and on the free x402
// gateway. One quick retry with a brief pause fixes the vast majority.
static void doOneRequestWithRetry(const char *method,
                                  const String &url,
                                  const String &jsonBody,
                                  const String &authBearer,
                                  const String &paymentSigHeader,
                                  int    &outStatus,
                                  String &outBody,
                                  String &outPayReq) {
  constexpr int MAX_TRIES = 3;
  for (int attempt = 0; attempt < MAX_TRIES; ++attempt) {
    doOneRequest(method, url, jsonBody, authBearer, paymentSigHeader,
                 outStatus, outBody, outPayReq);

    // Success, clear semantic (402 is the x402 payment handshake), or
    // any 4xx other than rate-limit 429 — don't retry.
    if (outStatus == 200 || outStatus == 402) return;
    bool transient = (outStatus < 0) || (outStatus == 429) ||
                     (outStatus >= 500);
    if (!transient) return;
    if (attempt < MAX_TRIES - 1) {
      Serial.printf("x402: transient %d on try %d, retrying…\n",
                    outStatus, attempt + 1);
      vTaskDelay(400 / portTICK_PERIOD_MS);
    }
  }
}

static X402Result x402Request(const char  *method,
                              const String &url,
                              const String &jsonBody,
                              const String &authBearer) {
  X402Result r;
  if (WiFi.status() != WL_CONNECTED) {
    r.error = "no wifi";
    return r;
  }

  // ── Pass 1: send the request. TLS client is scoped to this function
  //     call so it's fully released before we allocate the RPC client
  //     (avoids holding two ~30 KB mbedTLS contexts concurrently). ──
  int    code    = -1;
  String body;
  String payReq;
  doOneRequestWithRetry(method, url, jsonBody, authBearer, String(),
                        code, body, payReq);

  if (code != 402) {
    r.status = code;
    r.body   = body;
    if (code != 200) r.error = "status " + String(code);
    return r;
  }

  // ── Pass 2: parse payment-required, build the signed Solana payment.
  //     buildPaymentPayload may allocate its own short-lived TLS client
  //     for the ATA RPC lookup; first-request TLS is already gone. ──
  JsonDocument pr;
  DeserializationError err = deserializeJson(
      pr, payReq, DeserializationOption::NestingLimit(32));
  if (err) {
    Serial.printf("x402: 402 parse err=%s len=%u\n",
                  err.c_str(), (unsigned)payReq.length());
    r.status = 402;
    r.error  = String("could not parse 402 payload: ") + err.c_str();
    return r;
  }

  String amountStr;
  String paymentHeader = buildPaymentPayload(pr, url, &amountStr);
  if (paymentHeader.length() == 0) {
    r.status = 402;
    r.error  = "failed to build Solana payment";
    return r;
  }

  // ── Pass 3: retry with the PAYMENT-SIGNATURE header. Fresh TLS client,
  //     previous payment-build clients already gone. Same retry wrap so
  //     transient gateway failures don't waste an x402 payment. ──
  int    code2 = -1;
  String body2;
  String payReq2;
  doOneRequestWithRetry(method, url, jsonBody, authBearer, paymentHeader,
                        code2, body2, payReq2);

  r.status = code2;
  r.body   = body2;
  if (code2 == 200) {
    r.costUsd = amountStr.toDouble() / 1e6;
  } else {
    r.error = "retry status " + String(code2);
  }
  return r;
}

X402Result x402Post(const String &url,
                    const String &jsonBody,
                    const String &authBearer) {
  return x402Request("POST", url, jsonBody, authBearer);
}

X402Result x402Get(const String &url, const String &authBearer) {
  return x402Request("GET", url, String(), authBearer);
}
