#include "memory.h"
#include "devcfg.h"
#include "wallet.h"
#include "base58.h"
#include "solana_tx.h"
#include "arweave.h"
#include "voice.h"
#include "secrets.h"
#include "netgate.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <vector>

// ---------------------------------------------------------------------------
// Config knobs
// ---------------------------------------------------------------------------
static constexpr size_t   MAX_USER_CHARS    = 220;   // cap per-exchange inputs so
static constexpr size_t   MAX_ASSIST_CHARS  = 320;   //   the full memo fits in a tx
static constexpr int      QUEUE_DEPTH       = 8;     // pending writes cap
static constexpr uint32_t WRITE_TASK_STACK  = 16384; // TLS + JSON + Solana tx
                                                       // build + base64 all at
                                                       // once — be generous

static constexpr uint8_t  MEMO_VERSION_V1   = 0x01;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static uint8_t           s_key[32] = {0};
static bool              s_keyReady = false;
static TaskHandle_t      s_task     = nullptr;
static QueueHandle_t     s_queue    = nullptr;

// Stats
static volatile uint32_t s_stored       = 0;
static volatile uint32_t s_pending      = 0;
static volatile uint32_t s_written      = 0;
static volatile uint32_t s_failed       = 0;
static volatile uint32_t s_lastWriteMs  = 0;

// Arweave stats (accessed from the write task + UI).
static volatile uint32_t s_arWritten    = 0;
static volatile uint32_t s_arFailed     = 0;
static volatile uint32_t s_arLastMs     = 0;
static String            s_arLastTxId   = "";

// Each queued job carries the already-encrypted + base64-wrapped memo
// content ready to paste into the Solana memo instruction.
struct WriteJob {
  // We store a small fixed-size char buffer rather than a String because
  // FreeRTOS queues need POD-like items.
  char     memoB64[900];       // base64 of version|nonce|tag|ciphertext
  uint16_t memoLen;
};

// ---------------------------------------------------------------------------
// RPC helpers (self-contained so memory.cpp doesn't depend on x402.cpp's
// private static helpers)
// ---------------------------------------------------------------------------
static String pickRpcUrl() {
  String k = String(HELIUS_API_KEY);
  if (!k.startsWith("PASTE-") && k.length() >= 10) {
    return "https://mainnet.helius-rpc.com/?api-key=" + k;
  }
  return "https://api.mainnet-beta.solana.com";
}

static bool rpcCall(const String &payload, String &outBody) {
  if (WiFi.status() != WL_CONNECTED) return false;

  // Memory writes run on a dedicated task and already defer on
  // voiceIsBusy elsewhere; the netgate adds a second layer of DRAM-floor
  // protection so we never open a TLS handshake when mbedTLS would
  // fail to allocate.
  NetGate gate("memory-rpc", NetGate::Priority::Normal);
  if (!gate.ok()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  String url = pickRpcUrl();
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  if (code != 200) {
    Serial.printf("memory: rpc HTTP %d\n", code);
    http.end();
    return false;
  }
  outBody = http.getString();
  http.end();
  return true;
}

static bool fetchBlockhashBytes(uint8_t out[32]) {
  String payload = R"({"jsonrpc":"2.0","id":1,"method":"getLatestBlockhash","params":[]})";
  String body;
  if (!rpcCall(payload, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const char *hash = doc["result"]["value"]["blockhash"] | (const char *)nullptr;
  if (!hash) return false;
  auto bytes = base58Decode(String(hash));
  if (bytes.size() != 32) return false;
  memcpy(out, bytes.data(), 32);
  return true;
}

static bool sendRawTransaction(const String &txB64, String &outSig) {
  String payload = "{\"jsonrpc\":\"2.0\",\"id\":1,"
                   "\"method\":\"sendTransaction\",\"params\":[\"";
  payload += txB64;
  payload += "\",{\"encoding\":\"base64\",\"preflightCommitment\":\"processed\","
             "\"skipPreflight\":true}]}";
  String body;
  if (!rpcCall(payload, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const char *sig = doc["result"] | (const char *)nullptr;
  if (!sig) {
    const char *err = doc["error"]["message"] | "unknown error";
    Serial.printf("memory: sendTransaction failed: %s\n", err);
    return false;
  }
  outSig = String(sig);
  return true;
}

// ---------------------------------------------------------------------------
// HKDF-SHA256 (inline). Espressif's mbedTLS build doesn't expose
// mbedtls_hkdf so we roll it out of HMAC-SHA256 manually. For our 32-byte
// output we only need one expand step, which makes this trivially short.
// ---------------------------------------------------------------------------
static bool hmacSha256(const uint8_t *key, size_t keyLen,
                       const uint8_t *data, size_t dataLen,
                       uint8_t out[32]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, md, /*hmac=*/1) != 0) { mbedtls_md_free(&ctx); return false; }
  int rc = mbedtls_md_hmac_starts(&ctx, key, keyLen);
  if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, data, dataLen);
  if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return rc == 0;
}

static bool hkdfSha256_32(const uint8_t *ikm, size_t ikmLen,
                          const uint8_t *info, size_t infoLen,
                          uint8_t out32[32]) {
  // Extract: PRK = HMAC(salt=zero, IKM). Salt is the empty default.
  static const uint8_t zeroSalt[32] = {0};
  uint8_t prk[32];
  if (!hmacSha256(zeroSalt, 32, ikm, ikmLen, prk)) return false;

  // Expand (single block since L = 32 = HashLen).
  // T(1) = HMAC(PRK, info || 0x01)
  std::vector<uint8_t> buf(infoLen + 1);
  if (infoLen > 0) memcpy(buf.data(), info, infoLen);
  buf[infoLen] = 0x01;
  return hmacSha256(prk, 32, buf.data(), buf.size(), out32);
}

// ---------------------------------------------------------------------------
// Crypto — AES-256-GCM encrypt/decrypt around the memo payload
// ---------------------------------------------------------------------------
static bool encryptMemoPayload(const String &plaintext, String &outBase64) {
  if (!s_keyReady) return false;
  size_t ptLen = plaintext.length();
  if (ptLen == 0 || ptLen > 600) return false;   // keep tx size bounded

  uint8_t nonce[12];
  esp_fill_random(nonce, sizeof(nonce));

  std::vector<uint8_t> ct(ptLen);
  uint8_t tag[16];

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_key, 256);
  if (rc != 0) { mbedtls_gcm_free(&gcm); return false; }
  rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
            ptLen, nonce, sizeof(nonce), nullptr, 0,
            (const uint8_t *)plaintext.c_str(), ct.data(),
            sizeof(tag), tag);
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return false;

  std::vector<uint8_t> blob(1 + 12 + 16 + ptLen);
  blob[0] = MEMO_VERSION_V1;
  memcpy(&blob[1],  nonce, 12);
  memcpy(&blob[13], tag,   16);
  memcpy(&blob[29], ct.data(), ptLen);

  size_t olen = 0;
  mbedtls_base64_encode(nullptr, 0, &olen, blob.data(), blob.size());
  std::vector<uint8_t> b64(olen + 1, 0);
  size_t written = 0;
  if (mbedtls_base64_encode(b64.data(), b64.size(), &written,
                            blob.data(), blob.size()) != 0) {
    return false;
  }
  outBase64 = String((const char *)b64.data());
  return true;
}

static bool decryptMemoPayload(const String &memoB64, String &outPlaintext) {
  if (!s_keyReady) return false;

  // getSignaturesForAddress sometimes prefixes the memo with `[N] ` where
  // N is the UTF-8 length. Strip it if present.
  String m = memoB64;
  int br = m.indexOf(']');
  if (m.length() > 2 && m[0] == '[' && br > 0 &&
      br + 1 < (int)m.length() && m[br + 1] == ' ') {
    m = m.substring(br + 2);
  }

  size_t inLen = m.length();
  if (inLen < 40) return false;
  std::vector<uint8_t> blob(inLen);
  size_t olen = 0;
  if (mbedtls_base64_decode(blob.data(), blob.size(), &olen,
                            (const uint8_t *)m.c_str(), inLen) != 0) {
    return false;
  }
  if (olen < 29 || blob[0] != MEMO_VERSION_V1) return false;

  const uint8_t *nonce = &blob[1];
  const uint8_t *tag   = &blob[13];
  const uint8_t *ct    = &blob[29];
  size_t ctLen         = olen - 29;
  std::vector<uint8_t> pt(ctLen);

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_key, 256);
  if (rc != 0) { mbedtls_gcm_free(&gcm); return false; }
  rc = mbedtls_gcm_auth_decrypt(&gcm,
            ctLen, nonce, 12, nullptr, 0,
            tag, 16, ct, pt.data());
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return false;

  outPlaintext = String((const char *)pt.data()).substring(0, ctLen);
  return true;
}

// ---------------------------------------------------------------------------
// Background write task
// ---------------------------------------------------------------------------
static void memoryWriteTask(void *) {
  for (;;) {
    WriteJob job;
    if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
    s_pending = uxQueueMessagesWaiting(s_queue);

    if (WiFi.status() != WL_CONNECTED || !walletCanSign()) {
      Serial.println("memory: write skipped (no wifi or no signer)");
      s_failed++;
      continue;
    }

    // ------------------------------------------------------------------
    // Memo leg — only runs if the user still has the on-chain memo
    // backend enabled. Default-off users skip this entirely (Arweave
    // alone is free and permanent).
    // ------------------------------------------------------------------
    if (devcfgMemoryEnabled()) {
      uint8_t blockhash[32];
      if (!fetchBlockhashBytes(blockhash)) {
        Serial.println("memory: blockhash fetch failed");
        s_failed++;
      } else {
        const uint8_t *pub = walletPubkeyBytes();
        if (!pub) {
          s_failed++;
        } else {
          String txB64 = solanaBuildMemoTxBase64(
              pub, blockhash,
              (const uint8_t *)job.memoB64, job.memoLen);
          if (txB64.length() == 0) {
            Serial.println("memory: tx build failed");
            s_failed++;
          } else {
            String sig;
            if (sendRawTransaction(txB64, sig)) {
              s_written++;
              s_lastWriteMs = millis();
              Serial.printf("memory: wrote (%u bytes memo) sig=%.16s…\n",
                            (unsigned)job.memoLen, sig.c_str());
            } else {
              s_failed++;
            }
          }
        }
      }
    }

    // ------------------------------------------------------------------
    // Arweave archive leg: upload the SAME encrypted blob to Arweave via
    // Irys. Runs after the memo write so a slow/failed Irys upload never
    // blocks the primary on-chain memo path. Under 100 KiB is free on
    // Irys, so this costs nothing for typical chat memos.
    //
    // Wait for the voice TTS fetch / playback to finish first — running
    // a second TLS session while the voice task is also doing HTTPS +
    // LittleFS writes peaks heap near ~20 KB free, which was crashing
    // the board on long replies.
    // ------------------------------------------------------------------
    if (devcfgArweaveEnabled()) {
      // Wait for the voice subsystem to finish BOTH its ElevenLabs fetch
      // and its playback before we open our own TLS session. Voice fetch
      // alone holds a ~30 KB mbedTLS context; if we open a second one in
      // parallel the ESP32-S3 OOMs (SSL error -32512, "Memory allocation
      // failed"). Plus a heap-floor check in case something else is
      // fragmenting the pool — defer rather than crash.
      uint32_t dl = millis() + 25000;
      while ((voiceIsBusy() || ESP.getFreeHeap() < 70000) && millis() < dl) {
        vTaskDelay(200 / portTICK_PERIOD_MS);
      }
      ArweaveTag tags[] = {
        { "Content-Type", "application/octet-stream" },
        { "App-Name",     "daemon" },
        { "App-Version",  "1" },
        { "Kind",         "chat-memory-v1" },
        { "Wallet",       walletPubkey() },
      };
      String txId;
      bool arOk = arweaveUpload((const uint8_t *)job.memoB64, job.memoLen,
                                tags, sizeof(tags) / sizeof(tags[0]),
                                txId);
      if (arOk) {
        s_arWritten++;
        s_arLastMs   = millis();
        s_arLastTxId = txId;
        Serial.printf("memory: archived to Arweave https://gateway.irys.xyz/%s\n",
                      txId.c_str());
      } else {
        s_arFailed++;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void memoryBegin() {
  // Derive the AES-256 chat key from the wallet seed via HKDF-SHA256.
  const uint8_t *seed = walletSeedBytes();
  if (!seed) {
    Serial.println("memory: no wallet seed — memory disabled");
    s_keyReady = false;
    return;
  }
  const char *info = "daemon-chat-mem-v1";
  if (!hkdfSha256_32(seed, 32,
                     (const uint8_t *)info, strlen(info), s_key)) {
    Serial.println("memory: HKDF failed");
    s_keyReady = false;
    return;
  }
  s_keyReady = true;

  if (!s_queue) s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(WriteJob));
  if (!s_task) {
    xTaskCreatePinnedToCore(memoryWriteTask, "mem", WRITE_TASK_STACK,
                            nullptr, 1, &s_task, 1);
  }
  Serial.println("memory: key derived + write task started");
}

bool memoryKeyReady() { return s_keyReady; }

void memoryRecordExchange(const String &userText, const String &assistantText) {
  if (!s_keyReady)  return;
  // Record the exchange if EITHER backend is enabled. The write task then
  // decides per-job whether to do the memo leg, the Arweave leg, or both.
  if (!devcfgMemoryEnabled() && !devcfgArweaveEnabled()) return;
  if (!s_queue)     return;

  // Clamp each side so the combined JSON fits in one tx comfortably.
  String u = userText;      if (u.length() > MAX_USER_CHARS)   u = u.substring(0, MAX_USER_CHARS - 1)   + "…";
  String a = assistantText; if (a.length() > MAX_ASSIST_CHARS) a = a.substring(0, MAX_ASSIST_CHARS - 1) + "…";

  JsonDocument jd;
  jd["u"] = u;
  jd["a"] = a;
  String payload;
  serializeJson(jd, payload);

  String memoB64;
  if (!encryptMemoPayload(payload, memoB64)) {
    Serial.println("memory: encrypt failed");
    return;
  }
  if (memoB64.length() >= sizeof(WriteJob::memoB64)) {
    Serial.println("memory: memo too large");
    return;
  }

  WriteJob job;
  memset(&job, 0, sizeof(job));
  memcpy(job.memoB64, memoB64.c_str(), memoB64.length() + 1);
  job.memoLen = (uint16_t)memoB64.length();

  // If the queue is full, drop the oldest so the newest wins.
  if (uxQueueSpacesAvailable(s_queue) == 0) {
    WriteJob discard;
    xQueueReceive(s_queue, &discard, 0);
    Serial.println("memory: queue full, dropped oldest");
  }
  if (xQueueSend(s_queue, &job, 0) == pdTRUE) {
    s_pending = uxQueueMessagesWaiting(s_queue);
  }
}

// Turn an encrypted base64 payload (same format as memo content) into up
// to 2 chat turns by decrypting + parsing the JSON wrapper. Shared by the
// Arweave and memo recall paths.
static int decryptToTurns(const String &b64,
                          MemoryTurn *out, int outCap, int outPos) {
  String plain;
  if (!decryptMemoPayload(b64, plain)) return 0;
  JsonDocument jd;
  if (deserializeJson(jd, plain)) return 0;
  const char *u = jd["u"] | (const char *)nullptr;
  const char *a = jd["a"] | (const char *)nullptr;
  int written = 0;
  if (u && outPos + written < outCap) {
    out[outPos + written].role = "user";
    out[outPos + written].text = u;
    written++;
  }
  if (a && outPos + written < outCap) {
    out[outPos + written].role = "model";
    out[outPos + written].text = a;
    written++;
  }
  return written;
}

// Pull recent encrypted blobs from Arweave (via Irys GraphQL) and decrypt
// them into chat turns. Much faster than the Solana RPC scan below — one
// GraphQL POST + N gateway GETs, each ~100 ms.
static int recallFromArweave(MemoryTurn *out, int maxTurns) {
  String owner = walletPubkey();
  if (owner.length() == 0) return 0;

  // Estimate: each exchange becomes up to 2 turns, so fetch ceil(max/2) txs.
  int maxItems = (maxTurns + 1) / 2;
  if (maxItems > 40) maxItems = 40;

  std::vector<ArweaveItem> items;
  if (!arweaveFetchRecent("daemon", "chat-memory-v1",
                          owner, maxItems, items)) {
    return 0;
  }
  s_stored = items.size();
  int written = 0;
  for (const auto &it : items) {
    if (written >= maxTurns) break;
    written += decryptToTurns(it.data, out, maxTurns, written);
  }
  Serial.printf("memory: recalled %d turns from %u Arweave items\n",
                written, (unsigned)items.size());
  return written;
}

// Legacy path — Solana Memo program scan. Kept as a fallback when Arweave
// is disabled so historical memos written before the switch are still
// visible, and so users can run memory without any Arweave dependency.
static int recallFromMemo(MemoryTurn *out, int maxTurns) {
  String pubkey = walletPubkey();
  if (pubkey.length() == 0) return 0;

  String payload = String("{\"jsonrpc\":\"2.0\",\"id\":1,"
                          "\"method\":\"getSignaturesForAddress\","
                          "\"params\":[\"") + pubkey + "\",{\"limit\":50}]}";
  String body;
  if (!rpcCall(payload, body)) return 0;

  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::NestingLimit(16))) return 0;
  JsonArrayConst sigs = doc["result"].as<JsonArrayConst>();
  if (sigs.isNull()) return 0;

  std::vector<String> memos;
  for (JsonObjectConst sig : sigs) {
    const char *m = sig["memo"] | (const char *)nullptr;
    if (!m || !*m) continue;
    memos.emplace_back(m);
  }
  std::reverse(memos.begin(), memos.end());
  s_stored = memos.size();

  int written = 0;
  for (const String &memo : memos) {
    if (written >= maxTurns) break;
    written += decryptToTurns(memo, out, maxTurns, written);
  }
  Serial.printf("memory: recalled %d turns from %u memos\n",
                written, (unsigned)memos.size());
  return written;
}

int memoryRecallTurns(MemoryTurn *out, int maxTurns) {
  if (!s_keyReady || !out || maxTurns <= 0) return 0;
  // Either backend being on counts as "memory enabled" from a recall
  // perspective — we still want to restore history on boot even if the
  // user has flipped the memo write path off and relies on Arweave.
  bool mem = devcfgMemoryEnabled();
  bool ar  = devcfgArweaveEnabled();
  if (!mem && !ar) return 0;

  // Prefer Arweave when enabled. If the Arweave recall returns nothing
  // (fresh device, or the user just switched backends), fall back to the
  // Solana memo scan so existing users don't appear to lose their history
  // after toggling over to the Arweave-only UI.
  if (ar) {
    int n = recallFromArweave(out, maxTurns);
    if (n > 0) return n;
    Serial.println("memory: arweave empty, falling back to memo scan");
  }
  return recallFromMemo(out, maxTurns);
}

void memoryGetStats(MemoryStats &out) {
  out.enabled         = devcfgMemoryEnabled();
  out.keyReady        = s_keyReady;
  out.stored          = s_stored;
  out.pending         = s_pending;
  out.written         = s_written;
  out.failed          = s_failed;
  out.lastWriteMs     = s_lastWriteMs;

  out.arweaveEnabled  = devcfgArweaveEnabled();
  out.arweaveWritten  = s_arWritten;
  out.arweaveFailed   = s_arFailed;
  out.arweaveLastMs   = s_arLastMs;
  out.arweaveLastTxId = s_arLastTxId;
}
