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

// Diagnostic — runs the pipeline up to (but not past) the approval modal,
// returns the screen args as JSON. Caller is the test harness only.
bool swap_dry_run_for_test(const char *from_sym, const char *to_sym,
                           double amount_ui, uint16_t slippage_bps,
                           char *out_json, size_t cap);

// ---------------------------------------------------------------------------
//  Phase 2a-swap (in progress): Jupiter /swap-instructions parser.
//  Returns the swap broken into its constituent instructions plus the
//  address-lookup-table list, so the caller can wrap each ix in
//  vault_execute and compose its own v0 message. Uses heap allocation —
//  call jup_swap_instructions_free on every successful build.
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t pubkey[32];
    bool    is_signer;
    bool    is_writable;
} jup_acct_t;

typedef struct {
    uint8_t      program_id[32];
    jup_acct_t  *accounts;       // heap-allocated, .account_count entries
    size_t       account_count;
    uint8_t     *data;           // heap-allocated, .data_len bytes (base64-decoded)
    size_t       data_len;
} jup_ix_t;

typedef struct {
    jup_ix_t   *setup;            // 0..N entries
    size_t      setup_count;
    jup_ix_t    swap;             // valid iff has_swap
    bool        has_swap;
    jup_ix_t    cleanup;          // valid iff has_cleanup
    bool        has_cleanup;
    uint8_t   (*alt_addresses)[32];  // contiguous, .alt_count entries
    size_t      alt_count;
    uint64_t    priority_lamports;
} jup_swap_instructions_t;

// Build a quote-driven /swap-instructions request and parse the response.
// `quote_json` is the verbatim JSON object Jupiter's /quote returned —
// callers usually have it from jup_get_quote internally. `user_pubkey`
// is base58 — the vault PDA when callers want vault-rooted swaps, or
// the device key for the legacy direct path. Returns true on success;
// on failure the struct is left in a clean state and the caller doesn't
// need to free.
bool jup_swap_instructions_build(const char *quote_json,
                                 const char *user_pubkey,
                                 jup_swap_instructions_t *out);

// Free heap-allocated arrays inside the struct. Idempotent.
void jup_swap_instructions_free(jup_swap_instructions_t *out);

// Diagnostic — exercises jup_swap_instructions_build against live Jupiter,
// using the vault PDA as user_pubkey. Returns a JSON summary of the parsed
// shape (per-bucket counts, ALT count, priority lamports). Used by the
// host harness only.
bool swap_ix_summary_for_test(const char *from_sym, const char *to_sym,
                              double amount_ui, uint16_t slippage_bps,
                              char *out_json, size_t cap);

#ifdef __cplusplus
}
#endif
