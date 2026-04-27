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
// payment path uses ~10; Jupiter swaps via vault_execute can have 50+ unique
// accounts inline before ALT partitioning pushes most of them out.
#define V2_MAX_ACCOUNTS 80
#define V2_MAX_IXS      8
#define V2_MAX_ALTS     6     // Jupiter routes referenced 3-4 ALTs in practice
#define V2_MSG_BUF      1280  // Solana hard cap is 1232 bytes; keep slack.

typedef struct {
    uint8_t pubkey[32];
    bool    is_signer;
    bool    is_writable;
    int8_t  alt_index;       // -1 = static keys; otherwise idx into in->alts[]
    uint8_t pos_in_alt;      // position in alts[alt_index].addresses[] (0..255)
} v2_key_t;

// Per-ALT emission scratch — built after ALT classification, consumed at emit.
// writable_indexes[] / readonly_indexes[] hold the position-in-table values
// (the bytes we ship in the on-wire address_table_lookups), and the same
// position determines runtime ordering of the ALT-loaded accounts.
typedef struct {
    uint8_t writable_indexes[V2_MAX_ACCOUNTS];
    uint8_t readonly_indexes[V2_MAX_ACCOUNTS];
    uint8_t writable_count;
    uint8_t readonly_count;
} v2_alt_emit_t;

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
    keys[*n].alt_index   = -1;
    keys[*n].pos_in_alt  = 0;
    return (int)(*n)++;
}

// For each non-signer key, look it up in the supplied ALTs. First match
// wins (deterministic by ALT order, then by position in that ALT). Signers
// must remain in the static keys list — the runtime requires their signing
// slot to be discoverable from message header counts alone.
static void v2_classify_alts(v2_key_t *keys, size_t nkeys,
                             const solana_alt_t *alts, size_t nalts)
{
    if (!alts || nalts == 0) return;
    for (size_t k = 0; k < nkeys; k++) {
        if (keys[k].is_signer) continue;
        for (size_t a = 0; a < nalts; a++) {
            const solana_alt_t *t = &alts[a];
            if (!t->addresses || t->address_count == 0) continue;
            for (size_t p = 0; p < t->address_count && p < 256; p++) {
                if (memcmp(keys[k].pubkey, t->addresses[p], 32) == 0) {
                    keys[k].alt_index  = (int8_t)a;
                    keys[k].pos_in_alt = (uint8_t)p;
                    goto next_key;
                }
            }
        }
        next_key:;
    }
}

// Stable in-place partition: move all alt_index==-1 entries to the front,
// ALT-indexed entries to the back. Returns the number of static keys.
// O(n²) but n is bounded by V2_MAX_ACCOUNTS, and we save ~3 KB of stack
// vs a scratch-buffer copy. The fee_payer is interned first and is
// signer+writable so it never gets ALT-classified — it stays at slot 0.
static size_t v2_partition_static(v2_key_t *keys, size_t nkeys) {
    size_t i = 0;
    while (i < nkeys) {
        if (keys[i].alt_index < 0) { i++; continue; }
        size_t j;
        for (j = i + 1; j < nkeys; j++) {
            if (keys[j].alt_index < 0) break;
        }
        if (j >= nkeys) break;
        // Rotate keys[i..j+1] right by 1: keys[j] -> keys[i], keys[i..j-1] -> keys[i+1..j].
        v2_key_t saved = keys[j];
        for (size_t k = j; k > i; k--) keys[k] = keys[k - 1];
        keys[i] = saved;
        i++;
    }
    return i;
}

// Sort the static-keys prefix [0..nstatic) into Solana's required order:
//   [signed-writable] [signed-readonly] [unsigned-writable] [unsigned-readonly]
// while keeping fee_payer pinned at index 0. Stable insertion sort.
static void v2_classify_sort(v2_key_t *keys, size_t nstatic) {
    if (nstatic < 2) return;
    for (size_t i = 2; i < nstatic; i++) {
        v2_key_t cur = keys[i];
        int cur_rank = (cur.is_signer ? 0 : 2) + (cur.is_writable ? 0 : 1);
        size_t j = i;
        while (j > 1) {
            v2_key_t pv = keys[j - 1];
            int pv_rank = (pv.is_signer ? 0 : 2) + (pv.is_writable ? 0 : 1);
            if (pv_rank <= cur_rank) break;
            keys[j] = pv;
            j--;
        }
        keys[j] = cur;
    }
}

// Build per-ALT emit metadata. Walks the ALT-indexed keys (keys[nstatic..nkeys])
// and groups them by alt_index, splitting into writable / readonly per their
// is_writable flag. The on-wire indexes are pos_in_alt values, in the order
// the keys appear after partition (== order of first reference in input ixs,
// thanks to stable partition).
static void v2_build_alt_emit(const v2_key_t *keys, size_t nkeys, size_t nstatic,
                              size_t nalts, v2_alt_emit_t *out)
{
    for (size_t a = 0; a < nalts; a++) {
        out[a].writable_count = 0;
        out[a].readonly_count = 0;
    }
    for (size_t i = nstatic; i < nkeys; i++) {
        int a = keys[i].alt_index;
        if (a < 0 || (size_t)a >= nalts) continue;
        if (keys[i].is_writable) {
            if (out[a].writable_count < V2_MAX_ACCOUNTS) {
                out[a].writable_indexes[out[a].writable_count++] = keys[i].pos_in_alt;
            }
        } else {
            if (out[a].readonly_count < V2_MAX_ACCOUNTS) {
                out[a].readonly_indexes[out[a].readonly_count++] = keys[i].pos_in_alt;
            }
        }
    }
}

// Index of `pk` in the keys table (full range).
static int v2_index_of(const v2_key_t *keys, size_t n, const uint8_t pk[32]) {
    for (size_t i = 0; i < n; i++) {
        if (memcmp(keys[i].pubkey, pk, 32) == 0) return (int)i;
    }
    return -1;
}

// Resolve a pubkey to its runtime account index — the index into the
// final loaded-accounts list, which is:
//   [static_keys] + sum(per-ALT writables) + sum(per-ALT readonlies)
// Returns -1 on not-found.
static int v2_runtime_idx(const v2_key_t *keys, size_t nkeys,
                          size_t nstatic,
                          const v2_alt_emit_t *alt_emit, size_t nalts,
                          size_t total_writable_alt,
                          const uint8_t pk[32])
{
    int k = v2_index_of(keys, nkeys, pk);
    if (k < 0) return -1;
    if (keys[k].alt_index < 0) {
        // Static keys: position in [0..nstatic) is the runtime index.
        return k;
    }
    int a = keys[k].alt_index;
    uint8_t pos = keys[k].pos_in_alt;
    if (keys[k].is_writable) {
        size_t offset = nstatic;
        for (int i = 0; i < a; i++) offset += alt_emit[i].writable_count;
        for (uint8_t p = 0; p < alt_emit[a].writable_count; p++) {
            if (alt_emit[a].writable_indexes[p] == pos) return (int)(offset + p);
        }
    } else {
        size_t offset = nstatic + total_writable_alt;
        for (int i = 0; i < a; i++) offset += alt_emit[i].readonly_count;
        for (uint8_t p = 0; p < alt_emit[a].readonly_count; p++) {
            if (alt_emit[a].readonly_indexes[p] == pos) return (int)(offset + p);
        }
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

    if (in->alt_count > V2_MAX_ALTS) {
        ESP_LOGE(TAG, "too many ALTs (%u > %u)",
                 (unsigned)in->alt_count, V2_MAX_ALTS);
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

    // ---- 2. classify ALT membership + partition + sort static keys ------
    v2_classify_alts(keys, nkeys, in->alts, in->alt_count);
    size_t nstatic = v2_partition_static(keys, nkeys);
    v2_classify_sort(keys, nstatic);

    v2_alt_emit_t alt_emit[V2_MAX_ALTS] = {0};
    v2_build_alt_emit(keys, nkeys, nstatic, in->alt_count, alt_emit);

    size_t total_writable_alt = 0;
    for (size_t i = 0; i < in->alt_count; i++) total_writable_alt += alt_emit[i].writable_count;

    // ---- 3. derive header counts (over static keys only) ----------------
    uint8_t num_required_sigs    = 0;
    uint8_t num_readonly_signed  = 0;
    uint8_t num_readonly_unsigned = 0;
    for (size_t i = 0; i < nstatic; i++) {
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

    wc_shortvec(&w, (uint16_t)nstatic);
    for (size_t i = 0; i < nstatic; i++) wc_bytes(&w, keys[i].pubkey, 32);

    wc_bytes(&w, in->blockhash, 32);

    // 2 prefix CU instructions + ix_count caller instructions.
    wc_shortvec(&w, (uint16_t)(2 + in->ix_count));

    int cb_idx = v2_runtime_idx(keys, nkeys, nstatic,
                                alt_emit, in->alt_count, total_writable_alt,
                                cb_program);
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
        int pidx = v2_runtime_idx(keys, nkeys, nstatic,
                                  alt_emit, in->alt_count, total_writable_alt,
                                  ix->program_id);
        if (pidx < 0) return -1;
        wc_u8(&w, (uint8_t)pidx);
        wc_shortvec(&w, (uint16_t)ix->account_count);
        for (size_t j = 0; j < ix->account_count; j++) {
            int aidx = v2_runtime_idx(keys, nkeys, nstatic,
                                      alt_emit, in->alt_count, total_writable_alt,
                                      ix->accounts[j].pubkey);
            if (aidx < 0) return -1;
            wc_u8(&w, (uint8_t)aidx);
        }
        wc_shortvec(&w, (uint16_t)ix->data_len);
        if (ix->data_len) wc_bytes(&w, ix->data, ix->data_len);
    }

    // Address-table lookups. Empty when in->alts == NULL (the x402 /
    // refill paths) — Jupiter swaps fill this in when /swap-instructions
    // returns deep routes that need account addresses resolved at
    // runtime. The writable / readonly index arrays were synthesized
    // above from the partition; we just emit them.
    if (!in->alts || in->alt_count == 0) {
        wc_shortvec(&w, 0);
    } else {
        wc_shortvec(&w, (uint16_t)in->alt_count);
        for (size_t i = 0; i < in->alt_count; i++) {
            const solana_alt_t *alt = &in->alts[i];
            const v2_alt_emit_t *em = &alt_emit[i];
            if (!alt->table_pubkey) { w.overflow = true; break; }
            wc_bytes(&w, alt->table_pubkey, 32);
            wc_shortvec(&w, (uint16_t)em->writable_count);
            if (em->writable_count) wc_bytes(&w, em->writable_indexes, em->writable_count);
            wc_shortvec(&w, (uint16_t)em->readonly_count);
            if (em->readonly_count) wc_bytes(&w, em->readonly_indexes, em->readonly_count);
        }
    }

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
    // Slot ordering follows the sorted accounts table: keys[0..num_required_sigs)
    // are the signers in account-table order. We hold signer_pubkey's secret;
    // any other slot is zeroed (gateway fills the fee_payer slot when they
    // differ; truly-other signers are unsupported in v2 today).
    int signer_slot = v2_index_of(keys, nkeys, in->signer_pubkey);
    if (signer_slot < 0 || signer_slot >= num_required_sigs) {
        ESP_LOGE(TAG, "v2 signer not in required-sig range (slot=%d)", signer_slot);
        return -1;
    }
    uint8_t tx[1 + V2_MAX_ACCOUNTS * 64 + V2_MSG_BUF];
    wc_t tw = { .buf = tx, .cap = sizeof tx };
    wc_shortvec(&tw, num_required_sigs);
    uint8_t zero_sig[64] = {0};
    for (uint8_t s = 0; s < num_required_sigs; s++) {
        if (s == signer_slot) wc_bytes(&tw, sig, 64);
        else                  wc_bytes(&tw, zero_sig, 64);
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
