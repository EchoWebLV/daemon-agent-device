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
