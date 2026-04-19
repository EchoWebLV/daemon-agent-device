#include "arweave.h"
#include "wallet.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <esp_system.h>
#include <vector>

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static constexpr const char *IRYS_UPLOAD_URL   = "https://node1.irys.xyz/tx/solana";
static constexpr const char *IRYS_GRAPHQL_URL  = "https://node1.irys.xyz/graphql";
static constexpr const char *IRYS_GATEWAY_BASE = "https://gateway.irys.xyz/";
static constexpr int HTTP_TIMEOUT_MS = 30000;

// ---------------------------------------------------------------------------
// SHA-384 helper (used by DeepHash)
// ---------------------------------------------------------------------------
static bool sha384(const uint8_t *data, size_t len, uint8_t out[48]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA384);
  if (!info) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_setup(&ctx, info, 0) != 0) { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_starts(&ctx);
  if (len > 0) mbedtls_md_update(&ctx, data, len);
  mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return true;
}

// ---------------------------------------------------------------------------
// DeepHash (Arweave's recursive SHA-384 scheme). This is the exact
// pre-signing hash specified by ANS-104:
//
//   deepHash(bytes) = SHA-384(SHA-384("blob" + len) + SHA-384(bytes))
//   deepHash(list)  = fold acc=SHA-384("list"+len) over items: acc=SHA-384(acc + deepHash(item))
//
// The signing input for a DataItem is a list of 8 blob chunks (see below).
// ---------------------------------------------------------------------------
struct DhChunk {
  const uint8_t *data;
  size_t         len;
};

static bool deepHashBlob(const uint8_t *data, size_t len, uint8_t out[48]) {
  char tagStr[48];
  int  tagLen = snprintf(tagStr, sizeof(tagStr), "blob%u", (unsigned)len);
  uint8_t tagHash[48], dataHash[48], combined[96];
  if (!sha384((const uint8_t *)tagStr, tagLen, tagHash)) return false;
  if (!sha384(data, len, dataHash))                     return false;
  memcpy(combined,      tagHash,  48);
  memcpy(combined + 48, dataHash, 48);
  return sha384(combined, 96, out);
}

static bool deepHashList(const DhChunk *chunks, size_t n, uint8_t out[48]) {
  char tagStr[48];
  int  tagLen = snprintf(tagStr, sizeof(tagStr), "list%u", (unsigned)n);
  uint8_t acc[48];
  if (!sha384((const uint8_t *)tagStr, tagLen, acc)) return false;

  for (size_t i = 0; i < n; ++i) {
    uint8_t h[48], combined[96];
    if (!deepHashBlob(chunks[i].data, chunks[i].len, h)) return false;
    memcpy(combined,      acc, 48);
    memcpy(combined + 48, h,   48);
    if (!sha384(combined, 96, acc)) return false;
  }
  memcpy(out, acc, 48);
  return true;
}

// ---------------------------------------------------------------------------
// Avro binary encoding for tags. ANS-104 tags are an Avro `array` of
// `record {bytes name; bytes value}`. We emit a single block + zero
// terminator. Integer lengths are zigzag-varint encoded.
// ---------------------------------------------------------------------------
static void avroVarint(uint64_t v, std::vector<uint8_t> &out) {
  while ((v & ~(uint64_t)0x7F) != 0) {
    out.push_back((uint8_t)((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back((uint8_t)v);
}

// Zigzag for non-negative values is simply `v << 1`. All our lengths are
// non-negative so we only need the positive path.
static void avroZigZagNonNeg(uint64_t v, std::vector<uint8_t> &out) {
  avroVarint(v << 1, out);
}

static std::vector<uint8_t> encodeTagsAvro(const ArweaveTag *tags,
                                           size_t numTags) {
  std::vector<uint8_t> out;
  if (numTags == 0) return out;

  // Single block of `numTags` items (positive count means no explicit
  // block-byte-size prefix).
  avroZigZagNonNeg((uint64_t)numTags, out);
  for (size_t i = 0; i < numTags; ++i) {
    const String &n = tags[i].name;
    const String &v = tags[i].value;
    avroZigZagNonNeg(n.length(), out);
    out.insert(out.end(), (const uint8_t *)n.c_str(),
                          (const uint8_t *)n.c_str() + n.length());
    avroZigZagNonNeg(v.length(), out);
    out.insert(out.end(), (const uint8_t *)v.c_str(),
                          (const uint8_t *)v.c_str() + v.length());
  }
  out.push_back(0x00);   // zero-count block terminator
  return out;
}

// ---------------------------------------------------------------------------
// ANS-104 binary DataItem layout. `sig_type = 2` (Solana/Ed25519) gives us
// SIG=64 bytes, OWNER=32 bytes. target and anchor are optional; we always
// include a 32-byte random anchor to prevent replay.
// ---------------------------------------------------------------------------
static void appendLE(std::vector<uint8_t> &out, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) out.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}

bool arweaveUpload(const uint8_t *data, size_t dataLen,
                   const ArweaveTag *tags, size_t numTags,
                   String &outTxId) {
  outTxId = "";
  if (!data || dataLen == 0) {
    Serial.println("arweave: empty payload");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("arweave: wifi not connected");
    return false;
  }
  if (!walletCanSign()) {
    Serial.println("arweave: wallet cannot sign");
    return false;
  }
  const uint8_t *owner = walletPubkeyBytes();
  if (!owner) {
    Serial.println("arweave: no owner pubkey");
    return false;
  }

  // ---- 1. Tags as Avro binary -------------------------------------------
  std::vector<uint8_t> tagsBin = encodeTagsAvro(tags, numTags);

  // ---- 2. Random 32-byte anchor (prevents replay) ------------------------
  uint8_t anchor[32];
  esp_fill_random(anchor, 32);

  // ---- 3. DeepHash signing input ----------------------------------------
  // The ANS-104 spec signs the DeepHash of this exact 8-chunk list.
  static const uint8_t  kTagDataItem[] = { 'd','a','t','a','i','t','e','m' };
  static const uint8_t  kTagVersion[]  = { '1' };
  static const uint8_t  kSigType[]     = { '2' };     // Solana Ed25519
  static const uint8_t *kEmpty         = (const uint8_t *)"";

  DhChunk chunks[8] = {
    { kTagDataItem, sizeof(kTagDataItem) },
    { kTagVersion,  sizeof(kTagVersion)  },
    { kSigType,     sizeof(kSigType)     },
    { owner,        32                    },
    { kEmpty,       0                     },     // target: absent
    { anchor,       32                    },
    { tagsBin.empty() ? kEmpty : tagsBin.data(), tagsBin.size() },
    { data,         dataLen               },
  };
  uint8_t dh[48];
  if (!deepHashList(chunks, 8, dh)) {
    Serial.println("arweave: deepHash failed");
    return false;
  }

  // ---- 4. Sign the 48-byte DeepHash with the wallet's Ed25519 key -------
  uint8_t signature[64];
  if (!walletSign(dh, 48, signature)) {
    Serial.println("arweave: walletSign failed");
    return false;
  }

  // ---- 5. Assemble the DataItem binary ----------------------------------
  // Rough total: 2 + 64 + 32 + 1 + 1 + 32 + 8 + 8 + tags + data.
  std::vector<uint8_t> item;
  item.reserve(148 + tagsBin.size() + dataLen);

  appendLE(item, 2, 2);                          // signature type (LE uint16)
  item.insert(item.end(), signature, signature + 64);
  item.insert(item.end(), owner,     owner + 32);
  item.push_back(0x00);                          // target absent
  item.push_back(0x01);                          // anchor present
  item.insert(item.end(), anchor, anchor + 32);
  appendLE(item, (uint64_t)numTags,       8);   // tag count
  appendLE(item, (uint64_t)tagsBin.size(), 8);  // tag bytes length
  item.insert(item.end(), tagsBin.begin(), tagsBin.end());
  item.insert(item.end(), data, data + dataLen);

  Serial.printf("arweave: uploading %u byte DataItem (%u tag bytes, %u data)\n",
                (unsigned)item.size(), (unsigned)tagsBin.size(),
                (unsigned)dataLen);

  // ---- 6. POST to Irys --------------------------------------------------
  WiFiClientSecure client;
  client.setInsecure();                          // Irys cert not pinned yet
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, IRYS_UPLOAD_URL)) {
    Serial.println("arweave: http.begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/octet-stream");
  uint32_t t0 = millis();
  int code = http.POST(item.data(), item.size());
  uint32_t dt = millis() - t0;
  String body = http.getString();
  http.end();

  Serial.printf("arweave: HTTP %d in %u ms (body %u b)\n",
                code, (unsigned)dt, (unsigned)body.length());

  if (code != 200 && code != 201) {
    Serial.printf("arweave: upload failed body=%s\n",
                  body.length() > 240 ? (body.substring(0, 240) + "…").c_str()
                                      : body.c_str());
    return false;
  }

  // ---- 7. Extract tx ID from the response --------------------------------
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("arweave: bad JSON: %s\n", err.c_str());
    return false;
  }
  outTxId = doc["id"].as<String>();
  if (outTxId.length() == 0) {
    Serial.println("arweave: response missing id field");
    return false;
  }
  return true;
}

bool arweaveUploadString(const String &data,
                         const ArweaveTag *tags, size_t numTags,
                         String &outTxId) {
  return arweaveUpload((const uint8_t *)data.c_str(), data.length(),
                       tags, numTags, outTxId);
}

// ---------------------------------------------------------------------------
// Recall — query Irys GraphQL for recent items by owner + tag filters,
// then pull each blob from the gateway. Returns items oldest → newest so
// the caller can stream them straight into chat history.
// ---------------------------------------------------------------------------
static bool httpsPost(const String &url, const String &contentType,
                      const String &body, String &outResponse, int &outCode) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", contentType);
  outCode = http.POST(body);
  outResponse = http.getString();
  http.end();
  return true;
}

static bool httpsGet(const String &url, String &outResponse, int &outCode) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  // The Irys gateway answers with 302 → arweave.net for bundled items;
  // without following the redirect we'd never see the actual payload.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return false;
  outCode = http.GET();
  outResponse = http.getString();
  http.end();
  return true;
}

bool arweaveFetchRecent(const String &appName, const String &kind,
                        const String &ownerPubkey,
                        int maxItems,
                        std::vector<ArweaveItem> &out) {
  out.clear();
  if (WiFi.status() != WL_CONNECTED) return false;
  if (maxItems <= 0) return true;

  // GraphQL body. Order DESC on Irys gives us the most recent first, then
  // we reverse at the end so callers get oldest → newest.
  String gql;
  gql.reserve(640);
  gql += "{\"query\":\"{ transactions("
         "first: ";
  gql += String(maxItems);
  gql += ", order: DESC, owners: [\\\"";
  gql += ownerPubkey;
  gql += "\\\"], tags: ["
         "{ name: \\\"App-Name\\\", values: [\\\"";
  gql += appName;
  gql += "\\\"] }, "
         "{ name: \\\"Kind\\\", values: [\\\"";
  gql += kind;
  gql += "\\\"] }"
         "]) { edges { node { id timestamp } } } }\"}";

  String resp;
  int code = 0;
  if (!httpsPost(IRYS_GRAPHQL_URL, "application/json", gql, resp, code)) {
    Serial.println("arweave: graphql http begin failed");
    return false;
  }
  if (code != 200) {
    Serial.printf("arweave: graphql HTTP %d\n", code);
    return false;
  }

  // Parse the edges array. ArduinoJson is fine for this shape — we only
  // care about id + timestamp per edge.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.printf("arweave: graphql bad JSON: %s\n", err.c_str());
    return false;
  }

  JsonArrayConst edges = doc["data"]["transactions"]["edges"].as<JsonArrayConst>();
  if (edges.isNull()) {
    Serial.println("arweave: no edges in graphql response");
    return true;   // not an error — just nothing stored yet
  }

  std::vector<ArweaveItem> temp;
  temp.reserve(edges.size());
  for (JsonVariantConst edge : edges) {
    ArweaveItem it;
    it.txId      = edge["node"]["id"].as<String>();
    it.timestamp = edge["node"]["timestamp"].as<uint32_t>();
    if (it.txId.length() == 0) continue;
    temp.push_back(it);
  }

  // Fetch the actual blob for each tx. We do them sequentially to keep
  // memory pressure low — typical recall is ~10-30 items, each under 1 KB.
  for (size_t i = 0; i < temp.size(); ++i) {
    String url = String(IRYS_GATEWAY_BASE) + temp[i].txId;
    String body;
    int gcode = 0;
    if (!httpsGet(url, body, gcode) || gcode < 200 || gcode >= 300) {
      Serial.printf("arweave: fetch %s -> HTTP %d (skipping)\n",
                    temp[i].txId.c_str(), gcode);
      continue;
    }
    temp[i].data = body;
    out.push_back(temp[i]);
  }

  // Flip order: oldest first so the chat history replays in sequence.
  std::reverse(out.begin(), out.end());
  return true;
}

// ---------------------------------------------------------------------------
// Self-test — uploads a tiny "hello" blob. Useful as a sanity check that
// the DeepHash + ANS-104 + Irys plumbing is wired up correctly on this
// build of the device.
// ---------------------------------------------------------------------------
bool arweaveSelfTest() {
  String payload = "daemon selftest " + String((uint32_t)millis());
  ArweaveTag tags[] = {
    { "Content-Type", "text/plain" },
    { "App-Name",     "daemon" },
    { "App-Version",  "1" },
    { "Kind",         "selftest" },
  };
  String id;
  bool ok = arweaveUploadString(payload, tags, 4, id);
  if (ok) {
    Serial.printf("arweave: selftest OK — https://gateway.irys.xyz/%s\n",
                  id.c_str());
  } else {
    Serial.println("arweave: selftest FAILED");
  }
  return ok;
}
