// ---------------------------------------------------------------------------
//  Solana v0 versioned-transaction builder for x402 USDC payments.
//
//  Produces a partially-signed VersionedTransaction with:
//    - Fee payer in the first sig slot (left as 64 NUL bytes — facilitator
//      fills it in before broadcast)
//    - ComputeBudget: SetComputeUnitLimit + SetComputeUnitPrice
//    - SPL TransferChecked (USDC amount, decimals = 6)
//    - Wallet signature in the second sig slot
//
//  Same wire shape as the Chrome extension's createPaymentPayload path.
//  Signs via wallet_sign() so the key never leaves wallet.c's private state.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *fee_payer;      // 32 bytes (facilitator pubkey)
    const uint8_t *wallet_owner;   // 32 bytes (our pubkey)
    const uint8_t *source_ata;     // 32 bytes (our USDC ATA)
    const uint8_t *dest_ata;       // 32 bytes (recipient USDC ATA)
    const uint8_t *mint;           // 32 bytes (USDC mint)
    const uint8_t *blockhash;      // 32 bytes
    uint64_t       amount_atomic;  // 1e-6 USDC units
    uint8_t        mint_decimals;  // USDC: 6
    uint32_t       cu_limit;       // compute-unit limit (Chrome ext uses 8000)
    uint64_t       cu_price_micro; // compute-unit price (microlamports, 1)
} solana_tx_input_t;

// Build, sign, and base64-encode a VersionedTransaction. Writes a
// NUL-terminated string into `out` and returns its length (not counting
// the NUL), or -1 on any failure (missing input, no signing key, buffer
// too small, etc.).
//
// Sizing: the raw tx is ~300 B; base64 encoding adds ~34%. 512 B of
// output capacity is plenty of headroom for a single-instruction
// transfer like this.
int solana_build_signed_tx_base64(const solana_tx_input_t *in,
                                  char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
