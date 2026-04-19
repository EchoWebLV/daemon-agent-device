// ============================================================================
//  On-chain encrypted chat memory.
//
//  When memory is enabled (via `devcfgMemoryEnabled`), each completed user
//  ↔ assistant exchange is:
//    1. Packed into a compact JSON ({"u":..., "a":...})
//    2. Encrypted with AES-256-GCM using a key derived from the wallet
//       seed via HKDF-SHA256(info="daemon-chat-mem-v1"). Only the wallet
//       holder can decrypt.
//    3. Wrapped as `version(1) | nonce(12) | tag(16) | ciphertext`,
//       base64-encoded, and written to Solana mainnet as a single-signer
//       Memo Program transaction.
//
//  On boot, the most recent memos are fetched via
//  `getSignaturesForAddress`, decrypted, and loaded back into Daemon's
//  chat history so conversations survive reboots / flashes / power loss.
//
//  Nothing readable is ever written on-chain; only the wallet holder can
//  reconstruct the plaintext. Metadata (timing, sender, message count) is
//  still public.
// ============================================================================
#pragma once
#include <Arduino.h>

struct MemoryTurn {
  String role;   // "user" or "model"
  String text;
};

struct MemoryStats {
  bool     enabled;
  bool     keyReady;          // wallet has a seed and HKDF succeeded
  uint32_t stored;            // memos found at last recall
  uint32_t pending;           // writes currently queued
  uint32_t written;           // successful writes since boot
  uint32_t failed;            // failed writes since boot
  uint32_t lastWriteMs;       // millis() of last success, 0 if none
};

// Derive key + start background write task. Safe to call even when the
// wallet has no signing key — memoryEnabled() will just stay false.
void memoryBegin();

bool memoryKeyReady();

// Enqueue a user/assistant exchange for encrypted on-chain storage.
// Returns immediately; actual writing happens on a FreeRTOS task.
void memoryRecordExchange(const String &userText, const String &assistantText);

// Blocking. Fetches recent signatures via RPC, pulls the memo field,
// decrypts every one we can, and writes up to `max` resulting turns
// (oldest → newest) into `out`. Returns the number of turns written.
int  memoryRecallTurns(MemoryTurn *out, int max);

void memoryGetStats(MemoryStats &out);
