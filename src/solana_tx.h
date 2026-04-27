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

// ---------------------------------------------------------------------------
// v2 path: generic v0-message builder over an arbitrary list of instructions.
// Used by the vault_execute path; the v1 entry above stays for swap.c during
// the Phase 2a-x402 → Phase 2a-swap transition.
// ---------------------------------------------------------------------------

// Per-account meta inside a single instruction.
typedef struct {
    const uint8_t *pubkey;     // 32 bytes, non-owning
    bool           is_signer;
    bool           is_writable;
} solana_ix_account_t;

// One Solana instruction (program ID + per-account metas + opaque data).
typedef struct {
    const uint8_t              *program_id;     // 32 bytes
    const solana_ix_account_t  *accounts;
    size_t                      account_count;
    const uint8_t              *data;
    size_t                      data_len;
} solana_ix_v2_t;

// Address-Lookup-Table entry. Carries the table's pubkey plus its full
// address list — the builder partitions ix accounts into static-keys vs
// ALT-indexed slots itself, and synthesizes the per-direction index
// arrays from the result. This matches the data Jupiter hands us from
// /swap-instructions (just the table addresses) plus what we read from
// each table account via getAccountInfo.
typedef struct {
    const uint8_t  *table_pubkey;         // 32 bytes
    const uint8_t (*addresses)[32];       // table contents, .address_count entries
    size_t          address_count;        // 0..256
} solana_alt_t;

typedef struct {
    const uint8_t        *fee_payer;       // 32 bytes (slot-0 signer)
    const uint8_t        *signer_pubkey;   // 32 bytes — our wallet (slot-1 signer)
    const uint8_t        *blockhash;       // 32 bytes
    const solana_ix_v2_t *ixs;
    size_t                ix_count;
    uint32_t              cu_limit;
    uint64_t              cu_price_micro;
    // Optional address tables. Pass NULL / 0 for the no-ALT path (every
    // account inline in the static keys table). When present, every
    // non-signer account that appears in any of these tables gets pushed
    // out of the static keys list into the ALT it was found in.
    const solana_alt_t *alts;
    size_t              alt_count;
} solana_tx_input_v2_t;

// Same wire shape as solana_build_signed_tx_base64: signed VersionedTransaction
// (slot-0 sig zeroed for the facilitator to fill, slot-1 sig from our wallet),
// base64-encoded into `out`. Returns length on success, -1 on overflow / missing
// input / sign failure.
//
// Internally builds the deduped account table (fee_payer + signer first, then
// all unique pubkeys from the instructions' programs + accounts), classifies
// signer/writable, derives the v0 header counts, emits the instruction list
// with per-instruction account indices, signs the message, splices the sig.
int solana_build_tx_v2_base64(const solana_tx_input_v2_t *in,
                              char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
