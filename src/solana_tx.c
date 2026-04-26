// ---------------------------------------------------------------------------
//  Solana v0 transaction builder. See solana_tx.h.
//
//  Message layout (accounts must be in the order the Solana runtime expects
//  for header bookkeeping to resolve correctly):
//
//      [0] fee_payer        signer    writable
//      [1] wallet_owner     signer    writable
//      [2] source_ata                 writable
//      [3] dest_ata                   writable
//      [4] mint             readonly
//      [5] token_program    readonly
//      [6] compute_budget   readonly
//
//  Header: 2 required sigs, 0 readonly signed, 3 readonly unsigned.
//  Instructions: SetComputeUnitLimit + SetComputeUnitPrice + TransferChecked.
// ---------------------------------------------------------------------------
#include "solana_tx.h"

#include <string.h>

#include "mbedtls/base64.h"
#include "esp_log.h"

#include "base58.h"
#include "wallet.h"

static const char *TAG = "solana_tx";

// ---------------------------------------------------------------------------
// Fixed program IDs
// ---------------------------------------------------------------------------
static const char *TOKEN_PROGRAM_B58  = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
static const char *COMPUTE_BUDGET_B58 = "ComputeBudget111111111111111111111111111111";

// Instruction discriminators (borsh u8 at the head of each data blob).
enum {
    CB_SET_CU_LIMIT  = 0x02,
    CB_SET_CU_PRICE  = 0x03,
    SPL_TRANSFER_CHK = 0x0C,
};

// ---------------------------------------------------------------------------
// Write-cursor primitives over a bounded buffer. All append_* functions
// bail out silently if there's not enough room; the caller checks the
// final cursor against capacity.
// ---------------------------------------------------------------------------
typedef struct { uint8_t *buf; size_t cap; size_t len; bool overflow; } wc_t;

static void wc_bytes(wc_t *w, const uint8_t *src, size_t n) {
    if (w->len + n > w->cap) { w->overflow = true; return; }
    memcpy(w->buf + w->len, src, n);
    w->len += n;
}
static void wc_u8(wc_t *w, uint8_t v)  { wc_bytes(w, &v, 1); }
static void wc_u32_le(wc_t *w, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = (uint8_t)(v >> (i * 8));
    wc_bytes(w, b, 4);
}
static void wc_u64_le(wc_t *w, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (i * 8));
    wc_bytes(w, b, 8);
}
// compact-u16 (shortvec). 1..3 bytes; high bit indicates "more".
static void wc_shortvec(wc_t *w, uint16_t v) {
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v != 0) byte |= 0x80;
        wc_u8(w, byte);
    } while (v != 0);
}

// ---------------------------------------------------------------------------
// Program-ID resolution — decode the two fixed base58 strings once per
// call. Both decodes must succeed; their failure means something changed
// about base58 in a breaking way and the whole build is untrustworthy.
// ---------------------------------------------------------------------------
static bool decode_programs(uint8_t token_program[32], uint8_t compute_budget[32]) {
    if (base58_decode(TOKEN_PROGRAM_B58,  token_program,  32) != 32) return false;
    if (base58_decode(COMPUTE_BUDGET_B58, compute_budget, 32) != 32) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Build the message bytes. Returns bytes written (<=cap) or -1 on overflow.
// ---------------------------------------------------------------------------
static int build_message(const solana_tx_input_t *in,
                         const uint8_t token_program[32],
                         const uint8_t compute_budget[32],
                         uint8_t *out, size_t out_cap) {
    wc_t w = { .buf = out, .cap = out_cap };

    // v0 message prefix (MSB set — 0x80 | version=0).
    wc_u8(&w, 0x80);

    // Header: num_required_signatures, num_readonly_signed, num_readonly_unsigned.
    wc_u8(&w, 2);
    wc_u8(&w, 0);
    wc_u8(&w, 3);

    // Account keys: 7 × 32 bytes.
    wc_shortvec(&w, 7);
    wc_bytes(&w, in->fee_payer,      32);
    wc_bytes(&w, in->wallet_owner,   32);
    wc_bytes(&w, in->source_ata,     32);
    wc_bytes(&w, in->dest_ata,       32);
    wc_bytes(&w, in->mint,           32);
    wc_bytes(&w, token_program,      32);
    wc_bytes(&w, compute_budget,     32);

    wc_bytes(&w, in->blockhash, 32);

    // 3 instructions: CU limit, CU price, TransferChecked.
    wc_shortvec(&w, 3);

    // SetComputeUnitLimit: program_idx=6 (compute_budget), no accounts,
    // data = [0x02][u32 LE] = 5 bytes.
    wc_u8(&w, 6);
    wc_shortvec(&w, 0);
    wc_shortvec(&w, 5);
    wc_u8(&w, CB_SET_CU_LIMIT);
    wc_u32_le(&w, in->cu_limit);

    // SetComputeUnitPrice: data = [0x03][u64 LE] = 9 bytes.
    wc_u8(&w, 6);
    wc_shortvec(&w, 0);
    wc_shortvec(&w, 9);
    wc_u8(&w, CB_SET_CU_PRICE);
    wc_u64_le(&w, in->cu_price_micro);

    // TransferChecked: program_idx=5 (token_program),
    // accounts = [source_ata(2), mint(4), dest_ata(3), owner(1)],
    // data = [0x0C][u64 LE amount][u8 decimals] = 10 bytes.
    wc_u8(&w, 5);
    wc_shortvec(&w, 4);
    wc_u8(&w, 2);
    wc_u8(&w, 4);
    wc_u8(&w, 3);
    wc_u8(&w, 1);
    wc_shortvec(&w, 10);
    wc_u8(&w, SPL_TRANSFER_CHK);
    wc_u64_le(&w, in->amount_atomic);
    wc_u8(&w, in->mint_decimals);

    // Address-table lookups: none.
    wc_shortvec(&w, 0);

    return w.overflow ? -1 : (int)w.len;
}

// ---------------------------------------------------------------------------
// Public entry
// ---------------------------------------------------------------------------
int solana_build_signed_tx_base64(const solana_tx_input_t *in,
                                  char *out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return -1;
    if (!in->fee_payer || !in->wallet_owner || !in->source_ata ||
        !in->dest_ata || !in->mint || !in->blockhash) {
        ESP_LOGE(TAG, "missing input bytes");
        return -1;
    }
    if (!wallet_can_sign()) {
        ESP_LOGE(TAG, "wallet has no signing key");
        return -1;
    }

    uint8_t token_program[32], compute_budget[32];
    if (!decode_programs(token_program, compute_budget)) {
        ESP_LOGE(TAG, "program id decode failed");
        return -1;
    }

    // Upper bound on the message: v0 prefix(1) + header(3) + sv(1) +
    // 7*32 accounts + 32 blockhash + sv(1) + 3×(1 + sv + sv + data) ≈ 300 B.
    uint8_t msg[384];
    int msg_len = build_message(in, token_program, compute_budget,
                                msg, sizeof(msg));
    if (msg_len < 0) {
        ESP_LOGE(TAG, "message overflow");
        return -1;
    }

    uint8_t sig[64];
    if (!wallet_sign(msg, (size_t)msg_len, sig)) {
        ESP_LOGE(TAG, "sign failed");
        return -1;
    }

    // VersionedTransaction wire format:
    //   sig_count (shortvec = 2) + fee_payer_sig (64 zero) + wallet_sig (64)
    //   + message (msg_len bytes)
    uint8_t tx[1 + 64 + 64 + sizeof(msg)];
    wc_t tw = { .buf = tx, .cap = sizeof(tx) };
    wc_shortvec(&tw, 2);
    uint8_t zero_sig[64] = {0};
    wc_bytes(&tw, zero_sig, 64);
    wc_bytes(&tw, sig, 64);
    wc_bytes(&tw, msg, (size_t)msg_len);
    if (tw.overflow) { ESP_LOGE(TAG, "tx overflow"); return -1; }

    // Base64 into the caller's buffer.
    size_t written = 0;
    int rc = mbedtls_base64_encode((uint8_t *)out, out_cap, &written,
                                   tw.buf, tw.len);
    if (rc != 0) {
        ESP_LOGE(TAG, "base64 encode rc=%d (need %u have %u)",
                 rc, (unsigned)written, (unsigned)out_cap);
        return -1;
    }
    if (written >= out_cap) return -1;
    out[written] = '\0';
    return (int)written;
}

// ---------------------------------------------------------------------------
// v2: generic multi-instruction v0 message builder
// ---------------------------------------------------------------------------

// Caps the number of distinct accounts a single tx can reference. The vault
// payment path uses ~10; Jupiter swaps via vault_execute will be ~20-30.
#define V2_MAX_ACCOUNTS 32
#define V2_MAX_IXS      8
#define V2_MSG_BUF      1280   // Solana hard cap is 1232 bytes; keep slack.

typedef struct {
    uint8_t pubkey[32];
    bool    is_signer;
    bool    is_writable;
} v2_key_t;

// Returns the index of `pk` in keys[], adding it (with the given flags) if
// missing. ORs the flags on hit so the strictest classification wins. Returns
// -1 if the table is full.
static int v2_intern(v2_key_t *keys, size_t *n, const uint8_t pk[32],
                     bool is_signer, bool is_writable)
{
    for (size_t i = 0; i < *n; i++) {
        if (memcmp(keys[i].pubkey, pk, 32) == 0) {
            keys[i].is_signer   = keys[i].is_signer   || is_signer;
            keys[i].is_writable = keys[i].is_writable || is_writable;
            return (int)i;
        }
    }
    if (*n >= V2_MAX_ACCOUNTS) return -1;
    memcpy(keys[*n].pubkey, pk, 32);
    keys[*n].is_signer   = is_signer;
    keys[*n].is_writable = is_writable;
    return (int)(*n)++;
}

// After interning, sort into Solana's required order:
//     [signed-writable] [signed-readonly] [unsigned-writable] [unsigned-readonly]
// while keeping fee_payer at index 0 and signer_pubkey at index 1. We do the
// classification + sort in one pass with a stable-insertion shuffle.
static void v2_classify_sort(v2_key_t *keys, size_t n) {
    // Three-pass insertion sort: keep [0] and [1] fixed; sort the tail.
    // Class rank: 0 = signer+writable, 1 = signer-only, 2 = writable-only, 3 = readonly.
    // Lower rank goes earlier.
    if (n < 3) return;
    for (size_t i = 3; i < n; i++) {
        v2_key_t cur = keys[i];
        int cur_rank = (cur.is_signer ? 0 : 2) + (cur.is_writable ? 0 : 1);
        size_t j = i;
        while (j > 2) {
            v2_key_t pv = keys[j - 1];
            int pv_rank = (pv.is_signer ? 0 : 2) + (pv.is_writable ? 0 : 1);
            if (pv_rank <= cur_rank) break;
            keys[j] = pv;
            j--;
        }
        keys[j] = cur;
    }
}

// Index of `pk` in the (already-sorted) key table.
static int v2_index_of(const v2_key_t *keys, size_t n, const uint8_t pk[32]) {
    for (size_t i = 0; i < n; i++) {
        if (memcmp(keys[i].pubkey, pk, 32) == 0) return (int)i;
    }
    return -1;
}

int solana_build_tx_v2_base64(const solana_tx_input_v2_t *in,
                              char *out, size_t out_cap)
{
    if (!in || !out || out_cap == 0) return -1;
    if (!in->fee_payer || !in->signer_pubkey || !in->blockhash) return -1;
    if (in->ix_count > V2_MAX_IXS) {
        ESP_LOGE(TAG, "too many instructions (%u > %u)",
                 (unsigned)in->ix_count, V2_MAX_IXS);
        return -1;
    }
    if (!wallet_can_sign()) {
        ESP_LOGE(TAG, "wallet has no signing key");
        return -1;
    }

    // ---- 1. intern all accounts -----------------------------------------
    v2_key_t keys[V2_MAX_ACCOUNTS];
    size_t   nkeys = 0;
    if (v2_intern(keys, &nkeys, in->fee_payer,     true,  true)  < 0) goto oom;
    if (v2_intern(keys, &nkeys, in->signer_pubkey, true,  true)  < 0) goto oom;

    // Compute Budget program ID — needed for the SetCU prefix instructions.
    uint8_t cb_program[32];
    if (base58_decode(COMPUTE_BUDGET_B58, cb_program, 32) != 32) return -1;
    if (v2_intern(keys, &nkeys, cb_program, false, false) < 0) goto oom;

    for (size_t i = 0; i < in->ix_count; i++) {
        const solana_ix_v2_t *ix = &in->ixs[i];
        if (!ix->program_id) return -1;
        if (v2_intern(keys, &nkeys, ix->program_id, false, false) < 0) goto oom;
        for (size_t j = 0; j < ix->account_count; j++) {
            const solana_ix_account_t *a = &ix->accounts[j];
            if (!a->pubkey) return -1;
            if (v2_intern(keys, &nkeys, a->pubkey, a->is_signer, a->is_writable) < 0) goto oom;
        }
    }

    // ---- 2. order keys --------------------------------------------------
    v2_classify_sort(keys, nkeys);

    // ---- 3. derive header counts ----------------------------------------
    uint8_t num_required_sigs    = 0;
    uint8_t num_readonly_signed  = 0;
    uint8_t num_readonly_unsigned = 0;
    for (size_t i = 0; i < nkeys; i++) {
        if (keys[i].is_signer) {
            num_required_sigs++;
            if (!keys[i].is_writable) num_readonly_signed++;
        } else {
            if (!keys[i].is_writable) num_readonly_unsigned++;
        }
    }

    // ---- 4. emit message ------------------------------------------------
    uint8_t msg[V2_MSG_BUF];
    wc_t w = { .buf = msg, .cap = sizeof msg };

    wc_u8(&w, 0x80);                            // v0 marker
    wc_u8(&w, num_required_sigs);
    wc_u8(&w, num_readonly_signed);
    wc_u8(&w, num_readonly_unsigned);

    wc_shortvec(&w, (uint16_t)nkeys);
    for (size_t i = 0; i < nkeys; i++) wc_bytes(&w, keys[i].pubkey, 32);

    wc_bytes(&w, in->blockhash, 32);

    // 2 prefix CU instructions + ix_count caller instructions.
    wc_shortvec(&w, (uint16_t)(2 + in->ix_count));

    int cb_idx = v2_index_of(keys, nkeys, cb_program);
    if (cb_idx < 0) return -1;

    // SetComputeUnitLimit (program_idx, no accounts, [0x02][u32 LE])
    wc_u8(&w, (uint8_t)cb_idx);
    wc_shortvec(&w, 0);
    wc_shortvec(&w, 5);
    wc_u8(&w, CB_SET_CU_LIMIT);
    wc_u32_le(&w, in->cu_limit);

    // SetComputeUnitPrice ([0x03][u64 LE])
    wc_u8(&w, (uint8_t)cb_idx);
    wc_shortvec(&w, 0);
    wc_shortvec(&w, 9);
    wc_u8(&w, CB_SET_CU_PRICE);
    wc_u64_le(&w, in->cu_price_micro);

    // Caller instructions
    for (size_t i = 0; i < in->ix_count; i++) {
        const solana_ix_v2_t *ix = &in->ixs[i];
        int pidx = v2_index_of(keys, nkeys, ix->program_id);
        if (pidx < 0) return -1;
        wc_u8(&w, (uint8_t)pidx);
        wc_shortvec(&w, (uint16_t)ix->account_count);
        for (size_t j = 0; j < ix->account_count; j++) {
            int aidx = v2_index_of(keys, nkeys, ix->accounts[j].pubkey);
            if (aidx < 0) return -1;
            wc_u8(&w, (uint8_t)aidx);
        }
        wc_shortvec(&w, (uint16_t)ix->data_len);
        if (ix->data_len) wc_bytes(&w, ix->data, ix->data_len);
    }

    // No address-table lookups.
    wc_shortvec(&w, 0);

    if (w.overflow) {
        ESP_LOGE(TAG, "v2 message overflow");
        return -1;
    }

    // ---- 5. sign + assemble VersionedTransaction ------------------------
    uint8_t sig[64];
    if (!wallet_sign(msg, w.len, sig)) {
        ESP_LOGE(TAG, "v2 sign failed");
        return -1;
    }

    // tx = sig_count(shortvec) + sig_count*64 zero/sig + message
    uint8_t tx[1 + V2_MAX_ACCOUNTS * 64 + V2_MSG_BUF];
    wc_t tw = { .buf = tx, .cap = sizeof tx };
    wc_shortvec(&tw, num_required_sigs);
    uint8_t zero_sig[64] = {0};
    // Slot 0 = fee_payer (zeroed; gateway fills); slot 1 = our signature;
    // any other signers (we currently never have any) are also zeroed and
    // would need separate handling.
    for (uint8_t s = 0; s < num_required_sigs; s++) {
        if (s == 1) wc_bytes(&tw, sig, 64);
        else        wc_bytes(&tw, zero_sig, 64);
    }
    wc_bytes(&tw, msg, w.len);
    if (tw.overflow) { ESP_LOGE(TAG, "v2 tx overflow"); return -1; }

    size_t written = 0;
    int rc = mbedtls_base64_encode((uint8_t *)out, out_cap, &written,
                                   tw.buf, tw.len);
    if (rc != 0) {
        ESP_LOGE(TAG, "v2 base64 rc=%d (need %u have %u)",
                 rc, (unsigned)written, (unsigned)out_cap);
        return -1;
    }
    if (written >= out_cap) return -1;
    out[written] = '\0';
    return (int)written;

oom:
    ESP_LOGE(TAG, "account table full (%u keys)", V2_MAX_ACCOUNTS);
    return -1;
}
