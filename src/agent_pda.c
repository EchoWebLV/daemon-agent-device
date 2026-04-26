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

#include "esp_log.h"

static const char *TAG = "agent_pda";

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

// Implemented in Task 3 (next commit).
int agent_pda_derive_root(const uint8_t owner[32], uint8_t out_root[32]) {
    (void)owner; (void)out_root;
    ESP_LOGE(TAG, "agent_pda_derive_root: NOT IMPLEMENTED (Task 3)");
    return -1;
}

int agent_pda_derive_vault(const uint8_t agent_root[32], uint8_t out_vault[32]) {
    (void)agent_root; (void)out_vault;
    ESP_LOGE(TAG, "agent_pda_derive_vault: NOT IMPLEMENTED (Task 3)");
    return -1;
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
    (void)vault_pubkey; (void)current_signer; (void)inner_program_id;
    (void)inner_metas; (void)inner_meta_count;
    (void)inner_data; (void)inner_data_len;
    (void)out_ix_data; (void)out_ix_cap;
    (void)out_metas; (void)out_meta_count; (void)out_meta_cap;
    ESP_LOGE(TAG, "agent_pda_build_vault_execute_ix: NOT IMPLEMENTED (Task 4)");
    return -1;
}

// Suppress unused-static warnings until Task 3 wires this in.
__attribute__((unused)) static const char *_pda_marker(void) { return PDA_MARKER; }
