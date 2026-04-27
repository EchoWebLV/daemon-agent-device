// ---------------------------------------------------------------------------
//  Vault → device USDC ATA auto-refill.
//
//  The x402 hot-path drains the device's USDC ATA. This module tops it back
//  up out of the vault PDA via a vault_execute → SPL TransferChecked CPI
//  (separate tx — can't fit inside an x402 payment because the @x402/svm
//  validator only accepts memo/lighthouse instructions in the optional slots).
//
//  Triggered after each wallet_refresh — if the device's USDC ATA balance
//  drops below REFILL_THRESHOLD, a refill brings it back to REFILL_TARGET.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Target / threshold in micro-USDC (= 1e-6 USDC). 100_000 = $0.10 of float;
// refill kicks in when the on-chain device ATA balance falls below 30_000.
#define REFILL_TARGET_USDC_ATOMIC     100000ULL
#define REFILL_THRESHOLD_USDC_ATOMIC   30000ULL

// Inspect the current device USDC balance (cached by wallet_refresh) and
// fire a refill tx if it's below threshold. Caller is non-blocking — the
// actual RPC + signing happens here. Idempotent: if a refill is already
// in flight from a recent call, returns false without launching another.
//
// Returns true if a refill tx was submitted (signature in `out_txid`),
// false if no refill was needed or one was already in flight.
bool refill_check_and_maybe_run(char *out_txid, size_t txid_cap);

// Force a refill of `amount_atomic` micro-USDC regardless of current
// balance. Used by the test harness verb TEST VAULT REFILL.
bool refill_run_amount(uint64_t amount_atomic, char *out_txid, size_t txid_cap);

// Synchronous variant: submit, then poll getSignatureStatuses every ~800 ms
// up to ~30 s for confirmation, then trigger a wallet_refresh so that
// wallet_usdc_amount() reflects the new balance. Used by the swap path —
// when the device's USDC ATA is short, we top up out of the vault and wait
// for confirmation before deciding the swap can proceed.
bool refill_run_and_wait(uint64_t amount_atomic, char *out_txid, size_t txid_cap);

// Reverse direction: send `amount_atomic` micro-USDC from the device's USDC
// ATA into the vault's USDC ATA. No vault_execute needed — the device is
// the source authority, so a direct SPL TransferChecked is enough. Useful
// for sweeping swap winnings or excess float back into the bulk reserve.
bool vault_deposit_usdc(uint64_t amount_atomic, char *out_txid, size_t txid_cap);

#ifdef __cplusplus
}
#endif
