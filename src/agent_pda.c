// ---------------------------------------------------------------------------
//  Agent program client. See agent_pda.h for the public surface.
//
//  Internal layout (ordered top-to-bottom for ease of reading):
//     1. Constants (program IDs)
//     2. Anchor discriminator helper
//     3. PDA derivation (sha256 grind + on-curve check)
//     4. InnerIx Borsh encoding inside vault_execute
// ---------------------------------------------------------------------------
#include "agent_pda.h"

#include <stdio.h>
#include <string.h>

#include "ed25519.h"
#include "mbedtls/sha256.h"

// Solana sentinel appended after seeds during PDA derivation; off-curve
// points produced this way can't be confused with a regular pubkey.
static const char PDA_MARKER[] = "ProgramDerivedAddress";

// Decoded base58 of "9DJqU25ShsEXNisbzSNUPzaN6qiSbU9XiNL7eerqYPFf".
const uint8_t AGENT_PROGRAM_ID[32] = {
    0x7a, 0x04, 0xa1, 0xc5, 0x52, 0x95, 0xae, 0xd7,
    0x2c, 0xa8, 0xbc, 0xf1, 0x5b, 0x2b, 0x41, 0x07,
    0xe4, 0x50, 0x32, 0x66, 0x0f, 0x81, 0xbc, 0x0d,
    0x9f, 0x5e, 0xb3, 0xec, 0x32, 0x93, 0xa1, 0xa2,
};

// Decoded base58 of "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA".
const uint8_t SPL_TOKEN_PROGRAM_ID[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93,
    0xd9, 0xcb, 0xe1, 0x46, 0xce, 0xeb, 0x79, 0xac,
    0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91,
    0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9,
};

// Decoded base58 of "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL".
const uint8_t SPL_ATA_PROGRAM_ID[32] = {
    0x8c, 0x97, 0x25, 0x8f, 0x4e, 0x24, 0x89, 0xf1,
    0xbb, 0x3d, 0x10, 0x29, 0x14, 0x8e, 0x0d, 0x83,
    0x0b, 0x5a, 0x13, 0x99, 0xda, 0xff, 0x10, 0x84,
    0x04, 0x8e, 0x7b, 0xd8, 0xdb, 0xe9, 0xf8, 0x59,
};

// One PDA candidate: sha256(seeds || bump || program_id || marker), then
// reject if it lands on the Ed25519 curve (would collide with a real keypair).
// Returns 1 if `out` is a valid (off-curve) PDA, 0 otherwise.
static int try_pda(const uint8_t **seeds, const size_t *seed_lens, size_t nseeds,
                   const uint8_t program_id[32],
                   uint8_t bump, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (size_t i = 0; i < nseeds; i++) {
        mbedtls_sha256_update(&c, seeds[i], seed_lens[i]);
    }
    mbedtls_sha256_update(&c, &bump, 1);
    mbedtls_sha256_update(&c, program_id, 32);
    mbedtls_sha256_update(&c, (const uint8_t *)PDA_MARKER, sizeof(PDA_MARKER) - 1);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
    return ed25519_pk_is_on_curve(out) ? 0 : 1;
}

// Grind down from 255 to 0 to find the canonical bump (matching Anchor's
// `find_program_address`). Returns the bump on success, -1 on failure.
static int find_pda(const uint8_t **seeds, const size_t *seed_lens, size_t nseeds,
                    const uint8_t program_id[32], uint8_t out[32])
{
    for (int bump = 255; bump >= 0; bump--) {
        if (try_pda(seeds, seed_lens, nseeds, program_id, (uint8_t)bump, out)) {
            return bump;
        }
    }
    return -1;
}

int agent_pda_derive_root(const uint8_t owner[32], uint8_t out_root[32]) {
    static const uint8_t SEED_TAG[] = "agent";
    const uint8_t *seeds[]     = { SEED_TAG,            owner };
    const size_t   seed_lens[] = { sizeof SEED_TAG - 1, 32    };
    return find_pda(seeds, seed_lens, 2, AGENT_PROGRAM_ID, out_root);
}

int agent_pda_derive_vault(const uint8_t agent_root[32], uint8_t out_vault[32]) {
    static const uint8_t SEED_TAG[] = "vault";
    const uint8_t *seeds[]     = { SEED_TAG,            agent_root };
    const size_t   seed_lens[] = { sizeof SEED_TAG - 1, 32         };
    return find_pda(seeds, seed_lens, 2, AGENT_PROGRAM_ID, out_vault);
}

int agent_pda_derive_ata(const uint8_t owner[32], const uint8_t mint[32],
                         uint8_t out_ata[32])
{
    const uint8_t *seeds[]     = { owner, SPL_TOKEN_PROGRAM_ID, mint };
    const size_t   seed_lens[] = { 32,    32,                   32   };
    return find_pda(seeds, seed_lens, 3, SPL_ATA_PROGRAM_ID, out_ata);
}

// Anchor instruction discriminator: first 8 bytes of sha256("global:<name>").
// Cross-checked against `crypto.createHash("sha256").update("global:vault_execute")`
// which yields b2c50da8 9714ae28.
static void anchor_discriminator(const char *name, uint8_t out[8]) {
    char buf[64];
    int n = snprintf(buf, sizeof buf, "global:%s", name);
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t *)buf, (size_t)n, hash, 0);
    memcpy(out, hash, 8);
}

// Borsh-Vec encoded little-endian length prefix (4 bytes).
static inline void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int agent_pda_build_vault_execute_ix(
    const uint8_t vault_pubkey[32],
    const uint8_t current_signer[32],
    const uint8_t inner_program_id[32],
    const agent_meta_t *inner_metas,
    size_t inner_meta_count,
    const uint8_t *inner_data,
    size_t inner_data_len,
    uint8_t *out_ix_data,
    size_t out_ix_cap,
    agent_meta_t *out_metas,
    size_t *out_meta_count,
    size_t out_meta_cap)
{
    if (!vault_pubkey || !current_signer || !inner_program_id) return -1;
    if (inner_meta_count > 0 && !inner_metas) return -1;
    if (inner_data_len > 0 && !inner_data) return -1;
    if (inner_meta_count > 64)               return -1;   // sanity
    if (inner_data_len   > 1024)             return -1;   // sanity

    // ---- ix data: [discriminator(8)][InnerIx Borsh] -----------------------
    //   InnerIx { program_id: Pubkey, accounts: Vec<Meta>, data: Vec<u8> }
    //   Meta    { pubkey: Pubkey, is_signer: bool, is_writable: bool }
    uint8_t       *p   = out_ix_data;
    uint8_t       *end = out_ix_data + out_ix_cap;

    if ((size_t)(end - p) < 8) return -1;
    anchor_discriminator("vault_execute", p);
    p += 8;

    if ((size_t)(end - p) < 32) return -1;
    memcpy(p, inner_program_id, 32);
    p += 32;

    if ((size_t)(end - p) < 4 + inner_meta_count * 34u) return -1;
    put_u32_le(p, (uint32_t)inner_meta_count);
    p += 4;
    for (size_t i = 0; i < inner_meta_count; i++) {
        memcpy(p, inner_metas[i].pubkey, 32); p += 32;
        *p++ = inner_metas[i].is_signer   ? 1 : 0;
        *p++ = inner_metas[i].is_writable ? 1 : 0;
    }

    if ((size_t)(end - p) < 4 + inner_data_len) return -1;
    put_u32_le(p, (uint32_t)inner_data_len);
    p += 4;
    if (inner_data_len) {
        memcpy(p, inner_data, inner_data_len);
        p += inner_data_len;
    }

    int ix_data_len = (int)(p - out_ix_data);

    // ---- account metas for the OUTER vault_execute ------------------------
    //   [0]  vault          (writable, NOT signer at outer level — PDA)
    //   [1]  current_signer (signer)
    //   [2..N+1]  inner ix's accounts (signer flag stripped — PDA signs them
    //            via CPI seeds inside the program; writable carried through)
    //   [N+2]  inner program account (read-only)
    if (!out_metas || !out_meta_count) return -1;
    size_t need = 2 + inner_meta_count + 1;
    if (need > out_meta_cap) return -1;

    size_t k = 0;
    out_metas[k++] = (agent_meta_t){ .pubkey = vault_pubkey,    .is_signer = false, .is_writable = true  };
    out_metas[k++] = (agent_meta_t){ .pubkey = current_signer,  .is_signer = true,  .is_writable = false };
    for (size_t i = 0; i < inner_meta_count; i++) {
        out_metas[k++] = (agent_meta_t){
            .pubkey      = inner_metas[i].pubkey,
            .is_signer   = false,
            .is_writable = inner_metas[i].is_writable,
        };
    }
    out_metas[k++] = (agent_meta_t){ .pubkey = inner_program_id, .is_signer = false, .is_writable = false };
    *out_meta_count = k;

    return ix_data_len;
}

