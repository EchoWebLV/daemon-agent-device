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
static String pickRpcUrl() {
  String k = String(HELIUS_API_KEY);
  if (!k.startsWith("PASTE-") && k.length() >= 10) {
    return "https://mainnet.helius-rpc.com/?api-key=" + k;
  }
  return "https://api.mainnet-beta.solana.com";
}

// One attempt of the RPC call. `code` < 0 means a transport error
// (WiFiClientSecure can't open a socket, TLS handshake failure, etc.) — those
// happen intermittently on ESP32 after a few back-to-back HTTPS requests and
// almost always clear up on a retry with a fresh client.
static bool rpcCallOnce(const String &payload, String &outBody, int &outCode) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, pickRpcUrl())) { outCode = -999; return false; }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);
  outCode = http.POST(payload);
  if (outCode != 200) {
    http.end();
    return false;
  }
  outBody = http.getString();
  http.end();
  return true;
}

static bool rpcCall(const String &payload, String &outBody) {
  if (WiFi.status() != WL_CONNECTED) return false;
  int code = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (rpcCallOnce(payload, outBody, code)) return true;
    Serial.printf("x402: rpc %d (attempt %d/3)\n", code, attempt + 1);
    // Only retry on transport errors (negative codes); an actual HTTP status
    // like 400/429/500 won't be fixed by a retry and wastes time.
    if (code >= 0) return false;
    delay(200 + attempt * 300);
  }
  return false;
}

// Recent blockhash: returns 32 bytes (base58-decoded) or false.
// Each call fetches a fresh blockhash — reusing one across calls produces
// byte-identical transactions (same amount/source/dest/mint), which
// Solana rejects as a duplicate signature, which makes the x402
// facilitator return 402 on the retry.
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

// Find the destination's USDC associated token account via RPC.
// getTokenAccountsByOwner with mint filter returns the ATA if one exists.
// Returns empty String if the recipient has no USDC account set up —
// which means an x402 service is mis-configured, the payment will fail.
//
// Small LRU-ish cache keyed on the owner address. One slot was not enough:
// a single chat turn alternates between sol.blockrun.ai (the LLM gateway)
// and one or more x402 service payees, and a 1-slot cache forced a Helius
// RPC round trip on every hop — which starts failing with intermittent TLS
// socket errors. 4 slots covers the hot set comfortably.
static constexpr size_t ATA_CACHE_SLOTS = 4;
static String s_ataCacheOwner [ATA_CACHE_SLOTS];
static String s_ataCachePubkey[ATA_CACHE_SLOTS];
static size_t s_ataCacheNext = 0;

static String fetchUsdcAta(const String &ownerB58) {
  if (ownerB58.length() == 0) return String();
  for (size_t i = 0; i < ATA_CACHE_SLOTS; i++) {
    if (s_ataCacheOwner[i] == ownerB58 && s_ataCachePubkey[i].length() > 0) {
      return s_ataCachePubkey[i];
    }
  }
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
  String result = pk ? String(pk) : String();
  if (result.length() > 0) {
    s_ataCacheOwner [s_ataCacheNext] = ownerB58;
    s_ataCachePubkey[s_ataCacheNext] = result;
    s_ataCacheNext = (s_ataCacheNext + 1) % ATA_CACHE_SLOTS;
  }
  return result;
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
// Public API
// ---------------------------------------------------------------------------
//
// Single request path shared by x402Post / x402Get. `method` is "POST" or
// "GET". For GET, `jsonBody` is ignored and no Content-Type header is sent.
static X402Result x402Request(const char   *method,
                              const String &url,
                              const String &jsonBody,
                              const String &authBearer) {
  const bool isPost = (strcmp(method, "POST") == 0);
  X402Result r;
  if (WiFi.status() != WL_CONNECTED) {
    r.error = "no wifi";
    return r;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(60000);
  if (!http.begin(client, url)) {
    r.error = "http.begin failed";
    return r;
  }
  if (isPost) http.addHeader("Content-Type", "application/json");
  if (authBearer.length() > 0) {
    http.addHeader("Authorization", "Bearer " + authBearer);
  }

  // Accept the various spellings of the payment-required header. sol.
  // blockrun.ai uses lowercase `payment-required`; other facilitators
  // sometimes publish `x-payment-required` or the standards-track
  // `WWW-Authenticate: X402 requirements="..."` header.
  static const char *kPaymentHeaders[] = {
    "payment-required",
    "x-payment-required",
    "www-authenticate",
  };
  http.collectHeaders(kPaymentHeaders, 3);

  uint32_t t0 = millis();
  int code = isPost ? http.POST(jsonBody) : http.GET();
  Serial.printf("x402: %s %s -> %d in %lu ms\n",
                method, url.c_str(), code, (unsigned long)(millis() - t0));

  if (code == 402) {
    // Try every known place the facilitator might have put the payment
    // description: a couple of custom headers, the WWW-Authenticate
    // header (with its `requirements="..."` quoted-value), then finally
    // fall back to the response body.
    String rawDescJson;
    auto tryDecode = [&](const String &b64) {
      if (b64.length() == 0 || rawDescJson.length() > 0) return;
      size_t olen = 0;
      std::vector<uint8_t> dec(b64.length() + 4, 0);
      if (mbedtls_base64_decode(dec.data(), dec.size(), &olen,
            (const uint8_t *)b64.c_str(), b64.length()) == 0 && olen > 0) {
        rawDescJson = String((const char *)dec.data()).substring(0, olen);
      }
    };
    tryDecode(http.header("payment-required"));
    tryDecode(http.header("x-payment-required"));

    // WWW-Authenticate: X402 requirements="<base64>"
    String wa = http.header("www-authenticate");
    int q1 = wa.indexOf('"');
    int q2 = wa.lastIndexOf('"');
    if (q1 >= 0 && q2 > q1) tryDecode(wa.substring(q1 + 1, q2));

    // Always pull the body too — gives us a fallback and useful log text.
    String body = http.getString();
    http.end();
    if (rawDescJson.length() == 0) rawDescJson = body;

    // The x402 payment-required body includes a deep `extensions.bazaar`
    // schema (>10 nesting levels) which blows past ArduinoJson's default
    // nesting limit. Bump it generously; it's a bounded parse, not a
    // loop, so this is just a safety knob.
    JsonDocument pr;
    DeserializationError err = deserializeJson(
        pr, rawDescJson, DeserializationOption::NestingLimit(32));
    if (err) {
      Serial.printf("x402: 402 parse err=%s len=%u last=<%s>\n",
                    err.c_str(),
                    (unsigned)rawDescJson.length(),
                    rawDescJson.c_str() +
                      (rawDescJson.length() > 60 ? rawDescJson.length() - 60 : 0));
      r.status = 402;
      r.error  = String("could not parse 402 payload: ") + err.c_str();
      return r;
    }

    String amountStr;
    String paymentHeader = buildPaymentPayload(pr, url, &amountStr);
    if (paymentHeader.length() == 0) {
      r.status = 402;
      r.error = "failed to build Solana payment";
      return r;
    }

    // Retry with the payment header. WiFiClientSecure occasionally fails
    // the TLS handshake on the ESP32 (HTTPCode < 0) right after a previous
    // HTTPS call closed — so try up to 3 times with a fresh client, only
    // retrying on transport-level failures (negative codes). An actual HTTP
    // status like 400/402/500 means the server answered and we should honor
    // that answer, not retry.
    int code2 = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
      WiFiClientSecure client2;
      client2.setInsecure();
      HTTPClient http2;
      http2.setTimeout(60000);
      if (!http2.begin(client2, url)) {
        code2 = -998;
        Serial.printf("x402: retry begin failed (attempt %d/3)\n", attempt + 1);
        delay(200 + attempt * 300);
        continue;
      }
      if (isPost) http2.addHeader("Content-Type", "application/json");
      http2.addHeader("PAYMENT-SIGNATURE", paymentHeader);
      if (authBearer.length() > 0) {
        http2.addHeader("Authorization", "Bearer " + authBearer);
      }

      uint32_t t1 = millis();
      code2    = isPost ? http2.POST(jsonBody) : http2.GET();
      r.status = code2;
      r.body   = http2.getString();
      http2.end();
      Serial.printf("x402: retry -> %d in %lu ms%s\n",
                    code2, (unsigned long)(millis() - t1),
                    (code2 < 0 && attempt < 2) ? " (will retry)" : "");
      if (code2 >= 0) break;  // server answered, don't retry
      delay(200 + attempt * 300);
    }
    if (code2 == 200) {
      r.costUsd = amountStr.toDouble() / 1e6;
    } else {
      r.error = "retry status " + String(code2);
    }
    return r;
  }

  // No payment required or some other status.
  r.status = code;
  r.body   = http.getString();
  http.end();
  if (code != 200) r.error = "status " + String(code);
  return r;
}

X402Result x402Post(const String &url,
                    const String &jsonBody,
                    const String &authBearer) {
  return x402Request("POST", url, jsonBody, authBearer);
}

X402Result x402Get(const String &url,
                   const String &authBearer) {
  return x402Request("GET", url, String(), authBearer);
}
