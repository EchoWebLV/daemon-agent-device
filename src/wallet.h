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

// Decode SOLANA_KEY into bytes, derive and store the 32-byte public address.
// Returns false if the key is malformed.
bool walletBegin();

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

// Compact multi-line summary suitable for injection into Gemini's system
// prompt. Example output:
//   Your wallet: 9xHb...Qk3 (Solana mainnet).
//   Native SOL: 1.237 (~$254).
//   Tokens: 12,450 USDC, 0.5 mSOL, 42 BONK.
String walletContext(double solUsdPrice);
