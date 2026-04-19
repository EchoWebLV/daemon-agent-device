#include "wallet.h"
#include "secrets.h"
#include "base58.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Ed25519.h>   // rweather/Crypto
#include <Preferences.h>
#include <esp_system.h>    // esp_fill_random

// NVS namespace + key for the persisted Phantom-style 64-byte secret.
// Namespace stays inside the main "daemon" prefs so it survives across
// the same settings NVS partition that holds volume / personality / etc.
static constexpr const char *NVS_NS       = "daemon";
static constexpr const char *NVS_KEY_SEC  = "wallet_sec";  // 64-byte blob

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
// Key management helpers (NVS-backed)
// ---------------------------------------------------------------------------
// Build a full 64-byte Phantom-style secret from a 32-byte seed by
// deriving the ed25519 public key and concatenating seed || pubkey.
static void buildFullSecretFromSeed(const uint8_t seed[32],
                                    uint8_t outFull[64]) {
  memcpy(outFull, seed, 32);
  uint8_t pub[32];
  Ed25519::derivePublicKey(pub, seed);
  memcpy(outFull + 32, pub, 32);
}

// Generate a fresh 32-byte seed via the hardware TRNG and expand it into
// the 64-byte Phantom format. esp_fill_random() is backed by the ESP32's
// RNG peripheral, which is cryptographically secure as long as Wi-Fi or
// Bluetooth are active (they are on this device) — see the ESP-IDF docs.
static void generateFreshSecret(uint8_t outFull[64]) {
  uint8_t seed[32];
  esp_fill_random(seed, sizeof(seed));
  buildFullSecretFromSeed(seed, outFull);
}

// Persist the 64-byte secret to NVS under the "daemon" namespace.
static bool saveSecretToNvs(const uint8_t full[64]) {
  Preferences nvs;
  if (!nvs.begin(NVS_NS, false)) return false;
  size_t n = nvs.putBytes(NVS_KEY_SEC, full, 64);
  nvs.end();
  return n == 64;
}

// Read back the 64-byte secret. Returns false when the slot is missing
// (fresh device) or corrupt (wrong length).
static bool loadSecretFromNvs(uint8_t outFull[64]) {
  Preferences nvs;
  if (!nvs.begin(NVS_NS, true)) return false;
  bool has = nvs.isKey(NVS_KEY_SEC);
  if (!has) { nvs.end(); return false; }
  size_t n = nvs.getBytes(NVS_KEY_SEC, outFull, 64);
  nvs.end();
  return n == 64;
}

// Apply a 64-byte secret to the module state: store bytes, derive the
// public key + base58 address, flip s_ok. Shared by both the boot-load
// and the regenerate flows.
static void applySecret(const uint8_t full[64]) {
  s_secretBytes.assign(full, full + 64);
  memcpy(s_pubkeyBytes, full + 32, 32);
  s_pubkey = base58Encode(full + 32, 32);
  s_ok = true;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool walletBegin() {
  uint8_t full[64];

  if (loadSecretFromNvs(full)) {
    applySecret(full);
    Serial.printf("wallet: loaded from NVS — address %s\n", s_pubkey.c_str());
    return true;
  }

  // First-run flow: generate a fresh keypair and save it so this is a
  // one-time event. The user gets a genuine new Solana wallet on every
  // factory reset / fresh flash.
  generateFreshSecret(full);
  if (!saveSecretToNvs(full)) {
    Serial.println("wallet: NVS save FAILED — continuing in-RAM only");
    // Still apply so the device is at least usable until the next reboot.
  }
  applySecret(full);
  Serial.printf("wallet: generated fresh wallet — address %s\n", s_pubkey.c_str());
  return true;
}

String walletExportPrivateKeyBase58() {
  if (!s_ok || s_secretBytes.size() != 64) return "";
  return base58Encode(s_secretBytes.data(), 64);
}

bool walletGenerateNew() {
  uint8_t full[64];
  generateFreshSecret(full);
  if (!saveSecretToNvs(full)) {
    Serial.println("wallet: regenerate NVS save FAILED");
    return false;
  }
  // Reset derived state so balances / tokens / USDC ATA from the old
  // wallet don't leak into the new one's UI until the next refresh.
  s_solBalance    = 0.0;
  s_tokens.clear();
  s_usdcAta       = "";
  s_lastRefreshMs = 0;
  applySecret(full);
  Serial.printf("wallet: regenerated — new address %s\n", s_pubkey.c_str());
  return true;
}

bool walletCanSign() {
  return s_ok && s_secretBytes.size() == 64;
}

const uint8_t *walletPubkeyBytes() {
  return s_ok ? s_pubkeyBytes : nullptr;
}

const uint8_t *walletSeedBytes() {
  return (s_ok && s_secretBytes.size() == 64) ? s_secretBytes.data() : nullptr;
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
