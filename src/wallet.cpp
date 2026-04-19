#include "wallet.h"
#include "secrets.h"
#include "base58.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Ed25519.h>   // rweather/Crypto

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool                      s_ok = false;
static std::vector<uint8_t>      s_secretBytes;   // 64 (full) or 32 (pubkey only)
static uint8_t                   s_pubkeyBytes[32] = {0};
static String                    s_pubkey;
static double                    s_solBalance = 0.0;
static std::vector<TokenHolding> s_tokens;
static uint32_t                  s_lastRefreshMs = 0;

// Cached associated-token-account for USDC — populated from the token
// accounts RPC response so we don't have to derive it client-side.
static String                    s_usdcAta;

// Common token-mint → symbol map so the AI can name big holdings by ticker
// without us round-tripping metadata for every refresh. Add more as needed.
struct KnownMint { const char *mint; const char *symbol; uint8_t decimals; };
static const KnownMint KNOWN_MINTS[] = {
  { "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v", "USDC",  6 },
  { "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB", "USDT",  6 },
  { "mSoLzYCxHdYgdzU16g5QSh3i5K3z3KZK7ytfqcJm7So", "mSOL",  9 },
  { "7dHbWXmci3dT8UFYWYZweBLXgycu7Y3iL6trKn1Y7ARj", "stSOL", 9 },
  { "jtojtomepa8beP8AuQc6eXt5FriJwfFMwQx2v2f9mCL",  "JTO",   9 },
  { "JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN",  "JUP",   6 },
  { "EKpQGSJtjMFqKZ9KQanSqYXRcF8fBopzLHYxdM65zcjm", "WIF",   6 },
  { "DezXAZ8z7PnrnRJjz3wXBoRgixCa6xjnB7YaB1pPB263", "BONK",  5 },
};
static constexpr int N_KNOWN_MINTS = sizeof(KNOWN_MINTS)/sizeof(KNOWN_MINTS[0]);

static const char *symbolForMint(const String &mint) {
  for (int i = 0; i < N_KNOWN_MINTS; ++i) {
    if (mint == KNOWN_MINTS[i].mint) return KNOWN_MINTS[i].symbol;
  }
  return "";
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool walletBegin() {
  String raw = String(SOLANA_KEY);
  if (raw.startsWith("PASTE-") || raw.length() == 0) {
    Serial.println("wallet: no key configured");
    s_ok = false;
    return false;
  }

  s_secretBytes = base58Decode(raw);
  if (s_secretBytes.size() != 32 && s_secretBytes.size() != 64) {
    Serial.printf("wallet: key decode yielded %u bytes (expected 32 or 64)\n",
                  (unsigned)s_secretBytes.size());
    s_ok = false;
    return false;
  }

  // For a 64-byte Phantom-style export the public key is the tail 32 bytes.
  // For a 32-byte input we assume it's already the public address.
  const uint8_t *pub = (s_secretBytes.size() == 64)
                      ? &s_secretBytes[32]
                      : s_secretBytes.data();
  memcpy(s_pubkeyBytes, pub, 32);
  s_pubkey = base58Encode(pub, 32);
  s_ok = true;
  Serial.printf("wallet: address %s (key size %u bytes, canSign=%d)\n",
                s_pubkey.c_str(),
                (unsigned)s_secretBytes.size(),
                walletCanSign() ? 1 : 0);
  return true;
}

bool walletCanSign() {
  return s_ok && s_secretBytes.size() == 64;
}

const uint8_t *walletPubkeyBytes() {
  return s_ok ? s_pubkeyBytes : nullptr;
}

// Ed25519 signing — rweather's library takes the 32-byte seed separately
// from the 32-byte public key. Phantom/Solana secret keys pack them as
// seed || pubkey in the 64-byte blob.
bool walletSign(const uint8_t *data, size_t len, uint8_t sigOut[64]) {
  if (!walletCanSign()) return false;
  Ed25519::sign(sigOut,
                /*privateKey=*/s_secretBytes.data(),         // first 32 = seed
                /*publicKey=*/ s_secretBytes.data() + 32,    // last  32 = pub
                data, len);
  return true;
}

String walletPubkey()                  { return s_pubkey; }
double walletSolBalance()              { return s_solBalance; }
const std::vector<TokenHolding> &walletTokens() { return s_tokens; }

// ---- USDC helpers ---------------------------------------------------------
static const char *USDC_MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

double walletUsdcAmount() {
  for (const TokenHolding &t : s_tokens) {
    if (t.mint == USDC_MINT) return t.amount;
  }
  return 0.0;
}

String walletUsdcAta() { return s_usdcAta; }

String walletUsdcDisplayString() {
  if (!s_ok) return String("");
  double u = walletUsdcAmount();
  char buf[24];
  if (u >= 10000)      snprintf(buf, sizeof(buf), "USDC %.0f",  u);
  else if (u >= 100)   snprintf(buf, sizeof(buf), "USDC %.1f",  u);
  else if (u >= 1)     snprintf(buf, sizeof(buf), "USDC %.2f",  u);
  else if (u > 0)      snprintf(buf, sizeof(buf), "USDC %.4f",  u);
  else                 snprintf(buf, sizeof(buf), "USDC 0.00");
  return String(buf);
}

uint32_t walletLastRefreshAgeMs() {
  if (s_lastRefreshMs == 0) return UINT32_MAX;
  return millis() - s_lastRefreshMs;
}

// ---------------------------------------------------------------------------
// JSON-RPC helper
// ---------------------------------------------------------------------------
// Pick an RPC endpoint: Helius when a key is provided, otherwise Solana's
// free public RPC so Daemon can still read his balance out of the box.
static String rpcEndpoint() {
  String key = String(HELIUS_API_KEY);
  if (!key.startsWith("PASTE-") && key.length() >= 10) {
    return "https://mainnet.helius-rpc.com/?api-key=" + key;
  }
  return "https://api.mainnet-beta.solana.com";
}

static bool rpcCall(const String &payload, String &outBody) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = rpcEndpoint();
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);
  if (code != 200) {
    Serial.printf("wallet: rpc HTTP %d (endpoint=%s)\n",
                  code,
                  url.startsWith("https://mainnet.helius") ? "helius" : "public");
    http.end();
    return false;
  }
  outBody = http.getString();
  http.end();
  return true;
}

// ---------------------------------------------------------------------------
// Refresh: getBalance + getTokenAccountsByOwner
// ---------------------------------------------------------------------------
static void refreshSolBalance() {
  String payload = String("{\"jsonrpc\":\"2.0\",\"id\":1,"
                          "\"method\":\"getBalance\",\"params\":[\"") +
                   s_pubkey + "\"]}";
  String body;
  if (!rpcCall(payload, body)) return;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return;
  uint64_t lamports = doc["result"]["value"].as<uint64_t>();
  s_solBalance = (double)lamports / 1e9;
}

static void refreshTokens() {
  // getTokenAccountsByOwner with the SPL token program filter returns every
  // token account the wallet holds, parsed so we don't have to decode the
  // 165-byte account ourselves.
  const char *PGM = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
  String payload = String(
      "{\"jsonrpc\":\"2.0\",\"id\":1,"
      "\"method\":\"getTokenAccountsByOwner\",\"params\":[\"") +
      s_pubkey + "\",{\"programId\":\"" + PGM + "\"},"
      "{\"encoding\":\"jsonParsed\"}]}";

  String body;
  if (!rpcCall(payload, body)) return;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return;

  std::vector<TokenHolding> next;
  JsonArray accounts = doc["result"]["value"].as<JsonArray>();
  if (accounts.isNull()) { s_tokens.clear(); return; }

  String foundUsdcAta;
  for (JsonObject acct : accounts) {
    const char *ataAddr = acct["pubkey"] | "";
    JsonObject info = acct["account"]["data"]["parsed"]["info"];
    const char *mint = info["mint"] | "";
    JsonObject amt   = info["tokenAmount"];
    double ui        = amt["uiAmount"].as<double>();
    uint8_t dec      = amt["decimals"] | 0;

    // Record the USDC ATA even if the balance is zero — we still need to
    // spend from it when we have a positive balance and the RPC result
    // order isn't guaranteed.
    if (strcmp(mint, "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v") == 0) {
      foundUsdcAta = ataAddr;
    }

    if (ui <= 0.0) continue;                 // skip empty accounts
    TokenHolding h;
    h.mint     = mint;
    h.symbol   = symbolForMint(h.mint);
    h.amount   = ui;
    h.decimals = dec;
    next.push_back(h);
  }
  s_usdcAta = foundUsdcAta;

  // Keep the list short; top 10 by UI amount is plenty for a spoken AI.
  std::sort(next.begin(), next.end(),
            [](const TokenHolding &a, const TokenHolding &b){
              return a.amount > b.amount;
            });
  if (next.size() > 10) next.resize(10);
  s_tokens = std::move(next);
}

void walletRefresh() {
  if (!s_ok) return;
  refreshSolBalance();
  refreshTokens();
  s_lastRefreshMs = millis();
  Serial.printf("wallet: refresh done — %.4f SOL, %u token accounts\n",
                s_solBalance, (unsigned)s_tokens.size());
}

// --- Background task --------------------------------------------------
static TaskHandle_t     s_task    = nullptr;
static SemaphoreHandle_t s_trigger = nullptr;

static void walletTaskEntry(void *) {
  for (;;) {
    if (xSemaphoreTake(s_trigger, portMAX_DELAY) == pdTRUE) {
      walletRefresh();
    }
  }
}

static void ensureWalletTask() {
  if (s_task) return;
  s_trigger = xSemaphoreCreateBinary();
  // 10 KB — wallet refresh does TWO back-to-back TLS calls (getBalance +
  // getTokenAccountsByOwner) plus JSON parsing of a potentially-large
  // response, so we give it more headroom than the price task.
  xTaskCreatePinnedToCore(walletTaskEntry, "wallet", 10240, nullptr,
                          1, &s_task, 1);
}

void walletRequestRefresh() {
  ensureWalletTask();
  if (s_trigger) xSemaphoreGive(s_trigger);
}

// ---------------------------------------------------------------------------
// Context string for the AI
// ---------------------------------------------------------------------------
static String shortAddr(const String &p) {
  if (p.length() < 9) return p;
  return p.substring(0, 4) + "…" + p.substring(p.length() - 4);
}

String walletContext(double solUsdPrice) {
  if (!s_ok) {
    return "(No wallet key configured — Daemon has no holdings yet.)";
  }
  String out;
  out.reserve(256);
  out += "Your wallet address is ";
  out += s_pubkey;
  out += " (Solana mainnet).\n";
  out += "Native SOL balance: ";
  out += String(s_solBalance, 4);
  if (solUsdPrice > 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), " (~$%.2f)", s_solBalance * solUsdPrice);
    out += buf;
  }
  out += ".\n";
  if (s_tokens.empty()) {
    out += "SPL tokens: none.\n";
  } else {
    out += "SPL token holdings: ";
    for (size_t i = 0; i < s_tokens.size(); ++i) {
      if (i) out += ", ";
      const TokenHolding &t = s_tokens[i];
      char buf[48];
      // Cut decimals so the spoken line doesn't drone long numbers.
      double a = t.amount;
      if (a >= 1000)       snprintf(buf, sizeof(buf), "%.0f", a);
      else if (a >= 10)    snprintf(buf, sizeof(buf), "%.2f", a);
      else                 snprintf(buf, sizeof(buf), "%.4f", a);
      out += buf;
      out += " ";
      out += (t.symbol.length() ? t.symbol : shortAddr(t.mint));
    }
    out += ".\n";
  }
  return out;
}
