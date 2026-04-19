// ============================================================================
//  Daemon's Solana wallet — holds the public address, polls Helius for
//  balances, and hands a short human-readable summary to the AI module so
//  the system prompt always includes current holdings.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <vector>

struct TokenHolding {
  String   mint;       // base58 mint address
  String   symbol;     // may be empty if metadata isn't cached
  double   amount;     // UI amount (already divided by 10^decimals)
  uint8_t  decimals;
};

// Load (or generate) the device's Solana keypair. On first boot the NVS
// is empty and a fresh Ed25519 keypair is generated from the hardware
// RNG and persisted. Subsequent boots read the same key back. Returns
// false only if NVS is unusable (extremely rare).
bool walletBegin();

// Export the full 64-byte Phantom-style secret key as a base58 string
// (same format Phantom shows in "Export Private Key"). This is the key
// that lets someone sign transactions and decrypt on-chain memory — do
// NOT share it. Used by the web UI's reveal-key flow behind a warning.
String walletExportPrivateKeyBase58();

// Wipe the current key and generate a fresh Ed25519 keypair. The old
// wallet address becomes unreachable from this device (funds stuck
// unless the user backed up the old key), and AES-encrypted chat memory
// tied to the old seed becomes undecryptable. Returns false on NVS
// failure. After success the caller should reload everything that
// depends on the wallet (memory, x402 caches, on-screen balance, …) —
// typically by rebooting.
bool walletGenerateNew();

// Returns "" if walletBegin() failed.
String walletPubkey();

// Blocking HTTPS round-trip to Helius. Refreshes SOL balance and the list
// of SPL token accounts. Safe to call periodically (once a minute).
void walletRefresh();

// Fire-and-forget: asks the background wallet task to perform a refresh.
// Returns immediately, so the main loop never freezes during RPC calls.
void walletRequestRefresh();

// Most recent values from walletRefresh(). All read-only.
double walletSolBalance();
const std::vector<TokenHolding> &walletTokens();
uint32_t walletLastRefreshAgeMs();

// Convenience: USDC balance (SPL token, mint EPjFWdd5...) and a short,
// status-bar-friendly representation like "USDC 12.34".
double  walletUsdcAmount();
String  walletUsdcDisplayString();

// The associated-token-account address that holds this wallet's USDC.
// Cached from the last getTokenAccountsByOwner response. Empty string if
// the wallet has never held USDC (no ATA exists yet).
String  walletUsdcAta();

// 32-byte wallet public key (same bytes as base58-decoded walletPubkey()).
// Returns nullptr if the wallet was initialised with only a pubkey-shaped
// input that couldn't be parsed.
const uint8_t *walletPubkeyBytes();

// First 32 bytes of the Phantom-style secret export (the ed25519 seed).
// Used as the master secret for deriving other keys (chat-memory AES key,
// future per-service keys, etc). Returns nullptr if the wallet was
// loaded with pubkey-only input.
const uint8_t *walletSeedBytes();

// True when the wallet was initialised from a full 64-byte Phantom-style
// secret key and can therefore sign.
bool    walletCanSign();

// Sign `len` bytes with the wallet's ed25519 key. Writes a 64-byte
// signature into `sigOut`. Returns false if the wallet can't sign.
bool    walletSign(const uint8_t *data, size_t len, uint8_t sigOut[64]);

// Compact multi-line summary suitable for injection into Gemini's system
// prompt. Example output:
//   Your wallet: 9xHb...Qk3 (Solana mainnet).
//   Native SOL: 1.237 (~$254).
//   Tokens: 12,450 USDC, 0.5 mSOL, 42 BONK.
String walletContext(double solUsdPrice);
