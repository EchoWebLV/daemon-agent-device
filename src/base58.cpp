#include "base58.h"

static const char ALPHABET[] =
  "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static int alphaIdx(char c) {
  for (int i = 0; i < 58; ++i) if (ALPHABET[i] == c) return i;
  return -1;
}

std::vector<uint8_t> base58Decode(const String &s) {
  std::vector<uint8_t> out;
  if (s.length() == 0) return out;

  // Count leading '1' characters (each represents a leading zero byte).
  int leadingZeros = 0;
  int i = 0;
  while (i < (int)s.length() && s[i] == '1') { leadingZeros++; i++; }

  // Bignum stored as base-256 bytes, little-end-at-index-0 is messy, so we
  // keep big-endian and multiply with carry propagation.
  std::vector<uint8_t> b256(((s.length() - i) * 733) / 1000 + 1, 0);
  for (; i < (int)s.length(); ++i) {
    int idx = alphaIdx(s[i]);
    if (idx < 0) return {};                // invalid character → fail
    int carry = idx;
    for (int j = (int)b256.size() - 1; j >= 0; --j) {
      carry += 58 * b256[j];
      b256[j] = (uint8_t)(carry & 0xFF);
      carry >>= 8;
    }
    if (carry != 0) return {};             // overflow → fail
  }

  // Strip leading zeros inside the bignum output.
  int start = 0;
  while (start < (int)b256.size() && b256[start] == 0) ++start;

  out.reserve(leadingZeros + (b256.size() - start));
  for (int k = 0; k < leadingZeros; ++k) out.push_back(0);
  for (int k = start; k < (int)b256.size(); ++k) out.push_back(b256[k]);
  return out;
}

String base58Encode(const uint8_t *bytes, size_t len) {
  if (len == 0) return "";

  // Leading zero bytes → leading '1' characters.
  size_t leadingZeros = 0;
  while (leadingZeros < len && bytes[leadingZeros] == 0) leadingZeros++;

  // Convert 256-ary bignum → 58-ary. Size estimate: log(256)/log(58) ≈ 1.365
  size_t outSize = (len - leadingZeros) * 138 / 100 + 1;
  std::vector<uint8_t> b58(outSize, 0);
  size_t length = 0;

  for (size_t i = leadingZeros; i < len; ++i) {
    int carry = bytes[i];
    size_t j = 0;
    for (ssize_t k = (ssize_t)b58.size() - 1;
         (carry != 0 || j < length) && k >= 0; --k, ++j) {
      carry += 256 * b58[k];
      b58[k] = (uint8_t)(carry % 58);
      carry /= 58;
    }
    length = j;
  }

  // Skip zero-pad inside b58 buffer.
  size_t it = b58.size() - length;
  while (it < b58.size() && b58[it] == 0) ++it;

  String out;
  out.reserve(leadingZeros + (b58.size() - it));
  for (size_t k = 0; k < leadingZeros; ++k) out += '1';
  for (; it < b58.size(); ++it) out += ALPHABET[b58[it]];
  return out;
}
