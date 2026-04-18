// ============================================================================
//  Minimal base58 codec — just enough for Solana key handling.
//  The alphabet matches Bitcoin/Solana base58 (no 0, O, I, l).
// ============================================================================
#pragma once
#include <Arduino.h>
#include <vector>

// Decode a base58 string into bytes. Returns an empty vector on malformed
// input.
std::vector<uint8_t> base58Decode(const String &s);

// Encode `len` bytes of `bytes` as a base58 string.
String base58Encode(const uint8_t *bytes, size_t len);
