#include "wallet.h"
#include "secrets.h"
#include "base58.h"
#include "netgate.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Ed25519.h>   // rweather/Crypto
#include <Preferences.h>
#include <esp_system.h>    // esp_fill_random

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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

// Mutex guarding all mutable state above: s_tokens, s_usdcAta,
// s_solBalance, s_pubkey, s_lastRefreshMs. Acquired by the background
// wallet task on write, and by every public getter + walletContext()
// on read. Lazy-initialised on first use so walletBegin() can lock
// during its own NVS load.
static SemaphoreHandle_t         s_stateMutex = nullptr;

static void ensureStateMutex() {
  if (!s_stateMutex) s_stateMutex = xSemaphoreCreateMutex();
}

// RAII scoped-lock. On construction it blocks (up to timeoutMs) for the
// mutex; `ok()` tells you whether we actually got it. Releasing in the
// destructor means every early-return path stays correct without
// hand-written goto cleanup.
class WalletLock {
public:
  explicit WalletLock(uint32_t timeoutMs = 1000) : held_(false) {
    ensureStateMutex();
    if (s_stateMutex &&
        xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
      held_ = true;
    }
  }
  ~WalletLock() {
    if (held_ && s_stateMutex) xSemaphoreGive(s_stateMutex);
  }
  bool ok() const { return held_; }
private:
  bool held_;
};

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
// and the regenerate flows. Locks s_stateMutex so a readers don't see
// a half-updated pubkey / pubkeyBytes pair.
static void applySecret(const uint8_t full[64]) {
  WalletLock lk;
  if (!lk.ok()) return;
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
  {
    WalletLock lk;
    if (lk.ok()) {
      s_solBalance    = 0.0;
      s_tokens.clear();
      s_usdcAta       = "";
      s_lastRefreshMs = 0;
    }
  }
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

String walletPubkey() {
  WalletLock lk;
  return lk.ok() ? s_pubkey : String();
}

double walletSolBalance() {
  WalletLock lk;
  return lk.ok() ? s_solBalance : 0.0;
}

std::vector<TokenHolding> walletTokens() {
  WalletLock lk;
  if (!lk.ok()) return {};
  return s_tokens;                            // copy out under lock
}

size_t walletTokenCount() {
  WalletLock lk;
  return lk.ok() ? s_tokens.size() : 0;
}

// ---- USDC helpers ---------------------------------------------------------
static const char *USDC_MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

double walletUsdcAmount() {
  WalletLock lk;
  if (!lk.ok()) return 0.0;
  for (const TokenHolding &t : s_tokens) {
    if (t.mint == USDC_MINT) return t.amount;
  }
  return 0.0;
}

String walletUsdcAta() {
  WalletLock lk;
  return lk.ok() ? s_usdcAta : String();
}

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
// Pick an RPC endpoint: Helius when a key is provided, otherwise rotate
// through several public Solana RPCs so we don't pin all traffic to the
// single aggressively-throttled `api.mainnet-beta.solana.com`.
static const char *WALLET_PUBLIC_RPCS[] = {
  "https://api.mainnet-beta.solana.com",
  "https://solana-rpc.publicnode.com",
};
static constexpr int WALLET_N_PUBLIC_RPCS =
    sizeof(WALLET_PUBLIC_RPCS) / sizeof(WALLET_PUBLIC_RPCS[0]);

static String rpcEndpointRotating() {
  String key = String(HELIUS_API_KEY);
  if (!key.startsWith("PASTE-") && key.length() >= 10) {
    return "https://mainnet.helius-rpc.com/?api-key=" + key;
  }
  static uint32_t s_idx = 0;
  const char *ep = WALLET_PUBLIC_RPCS[s_idx % WALLET_N_PUBLIC_RPCS];
  s_idx++;
  return String(ep);
}

static bool rpcCall(const String &payload, String &outBody) {
  if (WiFi.status() != WL_CONNECTED) return false;

  constexpr int MAX_TRIES    = 3;
  constexpr uint32_t BACKOFF = 350;

  for (int attempt = 0; attempt < MAX_TRIES; ++attempt) {
    // Wallet refresh is Low priority — if the gate is busy (voice is
    // fetching + LLM is thinking, e.g.) we skip this attempt and the
    // next periodic refresh will pick it up.
    NetGate gate("wallet", NetGate::Priority::Low);
    if (!gate.ok()) continue;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = rpcEndpointRotating();
    http.setTimeout(15000);
    if (!http.begin(client, url)) continue;
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(payload);
    if (code == 200) {
      outBody = http.getString();
      http.end();
      return true;
    }
    http.end();

    bool retryable = (code == 429) || (code >= 500) || (code < 0);
    Serial.printf("wallet: rpc HTTP %d try %d (ep=%.40s) retry=%d\n",
                  code, attempt + 1, url.c_str(), retryable ? 1 : 0);
    if (!retryable) return false;
    vTaskDelay((BACKOFF * (attempt + 1)) / portTICK_PERIOD_MS);
  }
  return false;
}

// ---------------------------------------------------------------------------
// Refresh: getBalance + getTokenAccountsByOwner
// ---------------------------------------------------------------------------
static void refreshSolBalance() {
  // Snapshot the pubkey under lock so we don't race a regenerate.
  String pk;
  { WalletLock lk; if (lk.ok()) pk = s_pubkey; }
  if (pk.length() == 0) return;

  String payload = String("{\"jsonrpc\":\"2.0\",\"id\":1,"
                          "\"method\":\"getBalance\",\"params\":[\"") +
                   pk + "\"]}";
  String body;
  if (!rpcCall(payload, body)) return;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return;
  uint64_t lamports = doc["result"]["value"].as<uint64_t>();

  WalletLock lk;
  if (lk.ok()) s_solBalance = (double)lamports / 1e9;
}

static void refreshTokens() {
  // Snapshot pubkey under lock.
  String pk;
  { WalletLock lk; if (lk.ok()) pk = s_pubkey; }
  if (pk.length() == 0) return;

  // getTokenAccountsByOwner with the SPL token program filter returns every
  // token account the wallet holds, parsed so we don't have to decode the
  // 165-byte account ourselves.
  const char *PGM = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
  String payload = String(
      "{\"jsonrpc\":\"2.0\",\"id\":1,"
      "\"method\":\"getTokenAccountsByOwner\",\"params\":[\"") +
      pk + "\",{\"programId\":\"" + PGM + "\"},"
      "{\"encoding\":\"jsonParsed\"}]}";

  String body;
  if (!rpcCall(payload, body)) return;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return;

  std::vector<TokenHolding> next;
  JsonArray accounts = doc["result"]["value"].as<JsonArray>();
  if (accounts.isNull()) {
    // Empty list still needs to replace the stored one under lock.
    WalletLock lk;
    if (lk.ok()) { s_tokens.clear(); s_usdcAta = ""; }
    return;
  }

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

  // Keep the list short; top 10 by UI amount is plenty for a spoken AI.
  std::sort(next.begin(), next.end(),
            [](const TokenHolding &a, const TokenHolding &b){
              return a.amount > b.amount;
            });
  if (next.size() > 10) next.resize(10);

  // Single atomic swap of both cached fields under lock. Any reader
  // that was blocked on the mutex now sees the new fully-built state.
  WalletLock lk;
  if (lk.ok()) {
    s_usdcAta = foundUsdcAta;
    s_tokens  = std::move(next);
  }
}

void walletRefresh() {
  if (!s_ok) return;
  refreshSolBalance();
  refreshTokens();

  double   sol;
  size_t   nTok;
  uint32_t ts = millis();
  {
    WalletLock lk;
    if (lk.ok()) {
      s_lastRefreshMs = ts;
      sol  = s_solBalance;
      nTok = s_tokens.size();
    } else {
      sol  = 0.0;
      nTok = 0;
    }
  }
  Serial.printf("wallet: refresh done — %.4f SOL, %u token accounts\n",
                sol, (unsigned)nTok);
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
  // Snapshot everything we need under one lock so the string we build
  // reflects a single consistent point in time. Copying a handful of
  // Strings + a small vector out of shared state is cheap; holding the
  // lock across the whole build would block the wallet refresh task
  // longer than we need.
  bool   ok;
  String pubkey;
  double sol;
  std::vector<TokenHolding> tokens;
  {
    WalletLock lk;
    if (!lk.ok()) {
      return "(wallet: state locked — snapshot unavailable)";
    }
    ok     = s_ok;
    pubkey = s_pubkey;
    sol    = s_solBalance;
    tokens = s_tokens;
  }
  if (!ok) {
    return "(No wallet key configured — Daemon has no holdings yet.)";
  }

  String out;
  out.reserve(256);
  out += "Your wallet address is ";
  out += pubkey;
  out += " (Solana mainnet).\n";
  out += "Native SOL balance: ";
  out += String(sol, 4);
  if (solUsdPrice > 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), " (~$%.2f)", sol * solUsdPrice);
    out += buf;
  }
  out += ".\n";
  if (tokens.empty()) {
    out += "SPL tokens: none.\n";
  } else {
    out += "SPL token holdings: ";
    for (size_t i = 0; i < tokens.size(); ++i) {
      if (i) out += ", ";
      const TokenHolding &t = tokens[i];
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
