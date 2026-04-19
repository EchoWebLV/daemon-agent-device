// ============================================================================
//  Solana versioned-transaction builder for x402 USDC payments.
//
//  Produces a partially-signed VersionedTransaction (v0 message) with:
//    - Fee payer in the first signature slot (signature left as 64 zero
//      bytes — the x402 facilitator / server fills it in before submit)
//    - ComputeBudget: SetComputeUnitLimit + SetComputeUnitPrice
//    - SPL TransferChecked (USDC amount, decimals = 6)
//    - Wallet signature in the second slot
//
//  Mirrors the shape built by the Chrome extension's
//  createPaymentPayload / transaction.sign([keypair]) path.
// ============================================================================
#pragma once
#include <Arduino.h>

struct SolanaTxInput {
  const uint8_t *feePayer;       // 32 bytes, base58-decoded
  const uint8_t *walletOwner;    // 32 bytes (our wallet pubkey)
  const uint8_t *sourceAta;      // 32 bytes (our USDC ATA)
  const uint8_t *destAta;        // 32 bytes (recipient's USDC ATA)
  const uint8_t *mint;           // 32 bytes (USDC mint)
  const uint8_t *blockhash;      // 32 bytes
  uint64_t       amountAtomic;   // in 1e-6 USDC units
  uint8_t        mintDecimals;   // USDC: 6
  uint32_t       cuLimit;        // compute-unit limit (8_000 like extension)
  uint64_t       cuPriceMicro;   // compute-unit price in microlamports (1)
};

// Build and sign the transaction; returns a base64 string ready to wrap
// in the x402 PAYMENT-SIGNATURE payload. Empty string on failure.
String solanaBuildSignedTxBase64(const SolanaTxInput &in);

// Build a single-signer legacy transaction containing ONE Memo program
// instruction carrying `memoBytes`. We're the fee payer + only signer, so
// the returned base64 tx is ready to submit via sendTransaction RPC.
// Used by the on-chain memory module for encrypted chat log writes.
String solanaBuildMemoTxBase64(
    const uint8_t walletPub[32],
    const uint8_t blockhash[32],
    const uint8_t *memoBytes, size_t memoLen);
