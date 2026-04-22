// ---------------------------------------------------------------------------
//  Base58 codec. See base58.h for the public API.
//
//  Implementation ports the Bitcoin/Solana reference algorithm: treat the
//  input as a bignum, repeatedly multiply by 58 (encode) or 256 (decode),
//  and carry through a temporary buffer sized by the log-ratio bounds from
//  base58.h. Leading-zero preservation is handled separately because the
//  bignum representation collapses them.
// ---------------------------------------------------------------------------
#include "base58.h"

#include <string.h>
#include <sys/types.h>   // ssize_t

static const char ALPHABET[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// Small lookup for the reverse mapping. 0xFF means "not a base58 char".
static int alpha_idx(char c) {
    for (int i = 0; i < 58; ++i) {
        if (ALPHABET[i] == c) return i;
    }
    return -1;
}

int base58_decode(const char *in, uint8_t *out, size_t out_cap) {
    if (!in || !out) return -1;
    size_t in_len = strlen(in);
    if (in_len == 0) return 0;

    // Count leading '1's — each represents a leading zero byte in the output.
    size_t leading_zeros = 0;
    size_t i = 0;
    while (i < in_len && in[i] == '1') { leading_zeros++; i++; }

    // Bignum scratch, sized by the worst-case ratio. Use the caller's
    // buffer if it happens to be big enough (common case: 32-byte Solana
    // keys into a 32-byte buffer — the scratch need is 32 bytes exactly).
    size_t scratch_len = ((in_len - i) * 733) / 1000 + 1;
    uint8_t scratch_local[96];   // enough for 128-char base58 inputs
    uint8_t *scratch = scratch_local;
    if (scratch_len > sizeof(scratch_local)) return -1;  // not worth heap'ing
    memset(scratch, 0, scratch_len);

    // For each base58 digit, multiply the big-endian scratch by 58 and add
    // the digit value; propagate carry toward the MSB.
    for (; i < in_len; ++i) {
        int idx = alpha_idx(in[i]);
        if (idx < 0) return -1;                   // invalid char
        int carry = idx;
        for (ssize_t j = (ssize_t)scratch_len - 1; j >= 0; --j) {
            carry += 58 * scratch[j];
            scratch[j] = (uint8_t)(carry & 0xFF);
            carry >>= 8;
        }
        if (carry != 0) return -1;                // overflow (shouldn't happen)
    }

    // Strip leading zero bytes *inside* the scratch (the bignum is stored
    // big-endian; those zeros are not meaningful leading zeros in the
    // original input — those were the '1' chars we already counted).
    size_t start = 0;
    while (start < scratch_len && scratch[start] == 0) start++;

    size_t out_len = leading_zeros + (scratch_len - start);
    if (out_len > out_cap) return -1;

    memset(out, 0, leading_zeros);
    memcpy(out + leading_zeros, scratch + start, scratch_len - start);
    return (int)out_len;
}

int base58_encode(const uint8_t *bytes, size_t len, char *out, size_t out_cap) {
    if (!bytes || !out) return -1;
    if (len == 0) {
        if (out_cap < 1) return -1;
        out[0] = '\0';
        return 0;
    }

    // Leading zero bytes map directly to leading '1' characters.
    size_t leading_zeros = 0;
    while (leading_zeros < len && bytes[leading_zeros] == 0) leading_zeros++;

    // Scratch sized by the encode ratio. 32 bytes of input → ≤45 chars of
    // output, which fits comfortably in 64.
    size_t scratch_cap = (len - leading_zeros) * 138 / 100 + 1;
    uint8_t scratch_local[96];
    if (scratch_cap > sizeof(scratch_local)) return -1;
    memset(scratch_local, 0, scratch_cap);

    // For each input byte, multiply scratch by 256 and add the byte value.
    size_t length = 0;
    for (size_t idx = leading_zeros; idx < len; ++idx) {
        int carry = bytes[idx];
        size_t j = 0;
        for (ssize_t k = (ssize_t)scratch_cap - 1;
             (carry != 0 || j < length) && k >= 0; --k, ++j) {
            carry += 256 * scratch_local[k];
            scratch_local[k] = (uint8_t)(carry % 58);
            carry /= 58;
        }
        length = j;
    }

    // Walk past any leading zero *digits* in the scratch buffer.
    size_t it = scratch_cap - length;
    while (it < scratch_cap && scratch_local[it] == 0) ++it;

    size_t out_len = leading_zeros + (scratch_cap - it);
    if (out_len + 1 > out_cap) return -1;        // +1 for NUL

    for (size_t k = 0; k < leading_zeros; ++k) out[k] = '1';
    for (size_t k = 0; it < scratch_cap; ++k, ++it) {
        out[leading_zeros + k] = ALPHABET[scratch_local[it]];
    }
    out[out_len] = '\0';
    return (int)out_len;
}
