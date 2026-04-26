// ---------------------------------------------------------------------------
//  Agent program client — typed wrappers around the deployed Anchor program
//  at AGENT_PROGRAM_ID. Owns the on-chain identity bits: program ID, PDA
//  derivation (agent_root, vault), Anchor instruction discriminators, the
//  InnerIx Borsh encoder, and an instruction builder that wraps an arbitrary
//  inner instruction in vault_execute.
//
//  All callers stay byte-oriented — base58 lives only in wallet.c / x402.c
//  for human-readable logging.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Devnet program ID (base58: "9DJqU25ShsEXNisbzSNUPzaN6qiSbU9XiNL7eerqYPFf")
extern const uint8_t AGENT_PROGRAM_ID[32];

// SPL Token program ID (base58: "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA")
extern const uint8_t SPL_TOKEN_PROGRAM_ID[32];

// Account-meta input/output for instruction-building APIs.
// `pubkey` is non-owning; the caller must keep the pointed-to bytes alive
// until the resulting tx has been serialized.
typedef struct {
    const uint8_t *pubkey;     // 32 bytes
    bool           is_signer;
    bool           is_writable;
} agent_meta_t;

// Derive the agent_root PDA from an owner's pubkey.
// Returns the bump byte on success, or -1 if no off-curve point is found
// (vanishingly unlikely for a valid owner).
int agent_pda_derive_root(const uint8_t owner[32], uint8_t out_root[32]);

// Derive the vault PDA from the agent_root.
int agent_pda_derive_vault(const uint8_t agent_root[32], uint8_t out_vault[32]);

// Build the wire-format `vault_execute` instruction wrapping a single inner
// instruction. The outer instruction's data is written into `out_ix_data`,
// the outer account-meta list into `out_metas[]`.
//
// The outer meta layout the program expects, in order:
//     [0]  vault          (writable, NOT signer at outer level)
//     [1]  current_signer (signer)
//     [2..N+1]  inner instruction's accounts (signer flag stripped — PDA
//               signs them via CPI seeds inside the program)
//     [N+2]  inner program account (read-only)
//
// Buffer sizing — caller must provide:
//   `out_ix_data`  >= 8 + 32 + 4 + (32+1+1)*inner_meta_count + 4 + inner_data_len
//   `out_metas`    >= 2 + inner_meta_count + 1
//
// Returns the number of bytes written into `out_ix_data` on success, or -1
// on any sizing / sanity failure.
int agent_pda_build_vault_execute_ix(
    const uint8_t        vault_pubkey[32],
    const uint8_t        current_signer[32],
    const uint8_t        inner_program_id[32],
    const agent_meta_t  *inner_metas,
    size_t               inner_meta_count,
    const uint8_t       *inner_data,
    size_t               inner_data_len,
    uint8_t             *out_ix_data,
    size_t               out_ix_cap,
    agent_meta_t        *out_metas,
    size_t              *out_meta_count,
    size_t               out_meta_cap);

#ifdef __cplusplus
}
#endif
