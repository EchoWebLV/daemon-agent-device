#include "solana_tx.h"
#include "wallet.h"
#include "base58.h"

#include <mbedtls/base64.h>
#include <vector>

// ---------------------------------------------------------------------------
// Program IDs (fixed for all Solana mainnet activity)
// ---------------------------------------------------------------------------
static const char *TOKEN_PROGRAM_B58    = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
static const char *COMPUTE_BUDGET_B58   = "ComputeBudget111111111111111111111111111111";

// Instruction discriminators
static constexpr uint8_t CB_SET_CU_LIMIT  = 0x02;
static constexpr uint8_t CB_SET_CU_PRICE  = 0x03;
static constexpr uint8_t SPL_TRANSFER_CHK = 0x0C;

// ---------------------------------------------------------------------------
// Serialization primitives
// ---------------------------------------------------------------------------
static void appendBytes(std::vector<uint8_t> &out, const uint8_t *src, size_t n) {
  out.insert(out.end(), src, src + n);
}
static void appendU8(std::vector<uint8_t> &out, uint8_t v) {
  out.push_back(v);
}
// compact-u16 (shortvec) — 1..3 bytes.
static void appendShortVec(std::vector<uint8_t> &out, uint16_t v) {
  do {
    uint8_t byte = v & 0x7F;
    v >>= 7;
    if (v != 0) byte |= 0x80;
    out.push_back(byte);
  } while (v != 0);
}
static void appendU32LE(std::vector<uint8_t> &out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back((v >> (i * 8)) & 0xFF);
}
static void appendU64LE(std::vector<uint8_t> &out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back((v >> (i * 8)) & 0xFF);
}

// Decode a base58 pubkey string into a fixed 32-byte buffer. Returns false
// on malformed input.
static bool decodeB58Pubkey(const char *s, uint8_t out[32]) {
  auto v = base58Decode(String(s));
  if (v.size() != 32) return false;
  memcpy(out, v.data(), 32);
  return true;
}

// ---------------------------------------------------------------------------
// Build the v0 message bytes
// ---------------------------------------------------------------------------
// Account layout (order matters — enforced by Solana's message compilation):
//   [0] fee_payer        signer    writable
//   [1] wallet_owner     signer    writable
//   [2] source_ata                 writable
//   [3] dest_ata                   writable
//   [4] mint             readonly
//   [5] token_program    readonly
//   [6] compute_budget   readonly
//
// Header:
//   num_required_signatures = 2  (fee_payer + wallet_owner)
//   num_readonly_signed     = 0
//   num_readonly_unsigned   = 3  (mint + token_program + compute_budget)
static std::vector<uint8_t> buildMessage(const SolanaTxInput &in,
                                          const uint8_t tokenProgram[32],
                                          const uint8_t computeBudget[32]) {
  std::vector<uint8_t> m;
  m.reserve(400);

  // v0 message prefix (MSB set)
  appendU8(m, 0x80);

  // Header
  appendU8(m, 2);   // numRequiredSignatures
  appendU8(m, 0);   // numReadonlySigned
  appendU8(m, 3);   // numReadonlyUnsigned

  // Account keys: count + 7 × 32 bytes
  appendShortVec(m, 7);
  appendBytes(m, in.feePayer,       32);
  appendBytes(m, in.walletOwner,    32);
  appendBytes(m, in.sourceAta,      32);
  appendBytes(m, in.destAta,        32);
  appendBytes(m, in.mint,           32);
  appendBytes(m, tokenProgram,      32);
  appendBytes(m, computeBudget,     32);

  // Recent blockhash
  appendBytes(m, in.blockhash, 32);

  // Instructions: count then each compiled instruction
  appendShortVec(m, 3);

  // --- 1. ComputeBudget::SetComputeUnitLimit ---
  appendU8(m, 6);                       // programIdIndex = compute budget
  appendShortVec(m, 0);                 // no accounts
  // data: [discriminator (u8)] [limit (u32 LE)] → 5 bytes
  appendShortVec(m, 5);
  appendU8(m, CB_SET_CU_LIMIT);
  appendU32LE(m, in.cuLimit);

  // --- 2. ComputeBudget::SetComputeUnitPrice ---
  appendU8(m, 6);
  appendShortVec(m, 0);
  // data: [discriminator (u8)] [price (u64 LE)] → 9 bytes
  appendShortVec(m, 9);
  appendU8(m, CB_SET_CU_PRICE);
  appendU64LE(m, in.cuPriceMicro);

  // --- 3. SPL TokenProgram::TransferChecked ---
  appendU8(m, 5);                       // programIdIndex = token program
  // accounts: [source_ata, mint, dest_ata, owner] by account-list index
  appendShortVec(m, 4);
  appendU8(m, 2);                       // source_ata
  appendU8(m, 4);                       // mint
  appendU8(m, 3);                       // dest_ata
  appendU8(m, 1);                       // owner (our wallet)
  // data: [discriminator (u8)] [amount (u64 LE)] [decimals (u8)] → 10 bytes
  appendShortVec(m, 10);
  appendU8(m, SPL_TRANSFER_CHK);
  appendU64LE(m, in.amountAtomic);
  appendU8(m, in.mintDecimals);

  // Address table lookups: empty compact-array
  appendShortVec(m, 0);

  return m;
}

// ---------------------------------------------------------------------------
// Public entry
// ---------------------------------------------------------------------------
String solanaBuildSignedTxBase64(const SolanaTxInput &in) {
  if (!walletCanSign()) {
    Serial.println("solana_tx: wallet has no signing key");
    return String();
  }
  if (!in.feePayer || !in.walletOwner || !in.sourceAta || !in.destAta ||
      !in.mint || !in.blockhash) {
    Serial.println("solana_tx: missing input bytes");
    return String();
  }

  // Resolve fixed program IDs.
  uint8_t tokenProgram[32], computeBudget[32];
  if (!decodeB58Pubkey(TOKEN_PROGRAM_B58,  tokenProgram)  ||
      !decodeB58Pubkey(COMPUTE_BUDGET_B58, computeBudget)) {
    Serial.println("solana_tx: failed to decode program ids");
    return String();
  }

  // Build + sign the message.
  std::vector<uint8_t> msg = buildMessage(in, tokenProgram, computeBudget);
  uint8_t walletSig[64];
  if (!walletSign(msg.data(), msg.size(), walletSig)) {
    Serial.println("solana_tx: ed25519 sign failed");
    return String();
  }

  // Assemble the full VersionedTransaction:
  //   [sig_count (compact-u16) = 2][fee_payer_sig (zero 64)][wallet_sig][message]
  std::vector<uint8_t> tx;
  tx.reserve(msg.size() + 130);
  appendShortVec(tx, 2);
  tx.insert(tx.end(), 64, 0);        // fee_payer placeholder (server fills)
  appendBytes(tx, walletSig, 64);    // our signature
  appendBytes(tx, msg.data(), msg.size());

  // Base64-encode.
  size_t olen = 0;
  // mbedtls base64: call once to learn size, allocate, call again.
  mbedtls_base64_encode(nullptr, 0, &olen, tx.data(), tx.size());
  std::vector<uint8_t> b64(olen + 1, 0);
  size_t written = 0;
  int rc = mbedtls_base64_encode(b64.data(), b64.size(), &written,
                                 tx.data(), tx.size());
  if (rc != 0) {
    Serial.printf("solana_tx: base64 encode failed (%d)\n", rc);
    return String();
  }
  return String((const char *)b64.data());
}
