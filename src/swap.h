// ---------------------------------------------------------------------------
//  On-device Solana token swaps via Jupiter v6.
//
//  Single in-flight swap at a time, gated by a hardware hold-to-confirm
//  approval modal. The LLM calls this through the synthetic `swap_tokens`
//  tool registered in ai.c; user-typed swap requests reach here via the
//  same path because the LLM parses the intent.
//
//  swap_request() is synchronous from the caller's POV: it blocks the
//  calling task on an internal semaphore until the approval window
//  resolves AND the resulting on-chain swap is confirmed (or fails).
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SWAP_OK,
    SWAP_ERR_QUOTE,           // Jupiter /quote unreachable or no route
    SWAP_ERR_INSUFFICIENT,    // wallet doesn't hold enough of `from_sym`
    SWAP_ERR_CANCELLED,       // user released early, swiped, or 30s timeout
    SWAP_ERR_IN_PROGRESS,     // a swap is already running
    SWAP_ERR_BUILD,           // Jupiter /swap failed or response unparseable
    SWAP_ERR_SUBMIT,          // RPC sendTransaction rejected the tx
    SWAP_ERR_UNCONFIRMED,     // tx didn't land within 30s (txid still valid)
} swap_status_t;

typedef struct {
    swap_status_t status;
    char          txid[96];     // base58, "" on failure
    double        amount_in;    // UI units (already divided by 10^dec)
    double        amount_out;   // UI units, 0 on failure
    char          from_sym[12];
    char          to_sym[12];
    char          error_msg[64]; // short, LLM-readable
} swap_result_t;

// Synchronous: blocks the caller until the swap completes or fails. Safe
// to call from a non-LVGL task (the approval screen is opened via
// lv_async_call internally). Returns true if `out->status == SWAP_OK`.
//
// `slippage_bps == 0`        → use the per-pair default
// `slippage_bps` outside [10, 500] → silently clamped to the default
bool swap_request(const char *from_sym,
                  const char *to_sym,
                  double      amount_ui,
                  uint16_t    slippage_bps,
                  swap_result_t *out);

// Diagnostic — used by the test harness only. Returns true iff `sym`
// resolves; on success writes "<mint>:<decimals>" into `out`.
bool swap_resolve_for_test(const char *sym, char *out, size_t cap);

// Diagnostic — opens the approval modal with hard-coded numbers and
// reports the resolution. Used by the host harness only.
const char *swap_show_demo_for_test(void);

// Diagnostic: take an unsigned base64 v0 tx, run the signature splice,
// and verify the output decodes and the signature at our slot validates
// against the message bytes. Returns 0 on success, negative on failure.
// Used by the test harness only (TEST SWAP SIG <b64>).
int swap_test_sig_for_test(const char *in_b64, char *out_b64, size_t cap);

#ifdef __cplusplus
}
#endif
