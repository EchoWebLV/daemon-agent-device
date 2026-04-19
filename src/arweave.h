// ============================================================================
//  Irys (bundler) uploader — permanent decentralized storage on Arweave
//  paid directly from the on-device Solana wallet.
//
//  Each upload is wrapped as an ANS-104 DataItem, signed with the device's
//  Ed25519 key (signature type 2 = Solana), and POSTed to Irys node1. Irys
//  bundles the item into an Arweave transaction and returns an Arweave
//  tx ID immediately. Uploads under 100 KiB are free; larger payloads are
//  charged in SOL against a pre-funded Irys balance.
//
//  We reuse the wallet's existing signing primitive (walletSign) and
//  ed25519 public key (walletPubkeyBytes) — no new key material.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <vector>

struct ArweaveTag {
  String name;
  String value;
};

// Upload raw bytes. Tags are written as ANS-104 DataItem tags (e.g. set
// Content-Type so Arweave gateways serve the payload with a mime type;
// App-Name lets you filter your own uploads via GraphQL). Returns true on
// success and writes the 43-char Arweave tx ID into `outTxId`.
bool arweaveUpload(const uint8_t *data, size_t dataLen,
                   const ArweaveTag *tags, size_t numTags,
                   String &outTxId);

// Convenience wrapper: upload a UTF-8 string.
bool arweaveUploadString(const String &data,
                         const ArweaveTag *tags, size_t numTags,
                         String &outTxId);

// Quick self-test — uploads a tiny "hello from daemon" blob and prints the
// resulting tx ID over Serial. Useful as a serial command to verify the
// Irys path works end-to-end before enabling it for real memory writes.
bool arweaveSelfTest();

// One fetched Arweave item. `data` is the raw payload bytes, `timestamp`
// is the Irys bundle timestamp in seconds (NOT Arweave block time — the
// bundled tx may not have settled on Arweave yet, but it's already
// retrievable via the Irys gateway).
struct ArweaveItem {
  String   txId;
  String   data;          // raw bytes (may be binary)
  uint32_t timestamp;     // seconds
};

// Pull up to `maxItems` most-recent items uploaded by this wallet that
// match `appName` + `kind` tags. Returns oldest → newest. Used by the
// memory module to recall encrypted chat history on boot.
bool arweaveFetchRecent(const String &appName, const String &kind,
                        const String &ownerPubkey,
                        int maxItems,
                        std::vector<ArweaveItem> &out);
