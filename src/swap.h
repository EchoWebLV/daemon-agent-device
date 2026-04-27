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

#include "solana_tx.h"   // solana_ix_account_t — wrapped ix outputs feed the v2 builder

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

// Wrap a single Jupiter instruction in agent_program::vault_execute. Bridges
// the parser shape (jup_ix_t, inline pubkey arrays) to the v2 builder shape
// (solana_ix_account_t, pointer-based metas). Outputs:
//   - `out_ix_data[..ix_len]`     — the outer ix data (Anchor disc + InnerIx)
//   - `out_metas[..*out_meta_count]` — outer account metas, in this order:
//        [0]    vault          (writable, NOT signer at outer level)
//        [1]    current_signer (signer)
//        [2..]  inner ix's accounts (signer flag stripped — PDA signs them
//               via CPI seeds inside the program)
//        [last] inner program account (read-only)
//
// The metas' pubkey pointers reference `vault_pubkey`, `current_signer`,
// `src->program_id`, and `src->accounts[i].pubkey`. The caller MUST keep
// those bytes alive until the resulting tx is serialized.
//
// Buffer sizing — caller must provide:
//   `out_ix_cap`   >= 8 + 32 + 4 + (32+1+1)*src->account_count + 4 + src->data_len
//   `out_meta_cap` >= 2 + src->account_count + 1
//
// Returns ix-data length (bytes written) on success, -1 on failure.
int jup_ix_wrap_vault_execute(const jup_ix_t      *src,
                              const uint8_t        vault_pubkey[32],
                              const uint8_t        current_signer[32],
                              uint8_t             *out_ix_data,
                              size_t               out_ix_cap,
                              solana_ix_account_t *out_metas,
                              size_t              *out_meta_count,
                              size_t               out_meta_cap);

// Diagnostic — exercises jup_swap_instructions_build against live Jupiter,
// using the vault PDA as user_pubkey. Returns a JSON summary of the parsed
// shape (per-bucket counts, ALT count, priority lamports). Used by the
// host harness only.
bool swap_ix_summary_for_test(const char *from_sym, const char *to_sym,
                              double amount_ui, uint16_t slippage_bps,
                              char *out_json, size_t cap);

// Diagnostic — runs the parser AND the per-ix vault_execute wrapper against
// a live Jupiter response, returns a JSON shape summary including the
// outer ix-data lengths and outer meta counts for each parsed instruction.
// Used by the host harness only.
bool swap_ix_wrap_for_test(const char *from_sym, const char *to_sym,
                           double amount_ui, uint16_t slippage_bps,
                           char *out_json, size_t cap);

// ---------------------------------------------------------------------------
//  Phase 2a-swap: Address Lookup Table (ALT) contents fetch.
//
//  Jupiter's /swap-instructions response lists ALT addresses but not their
//  contents. To partition each ix's accounts into static-keys vs ALT-indexed
//  slots when building a v0 message, we have to fetch each ALT account
//  ourselves via getAccountInfo and decode its address list.
//
//  On-wire ALT account layout (Solana SDK):
//    bytes 0..56     LookupTableMeta header (fixed-width, even with no auth)
//    bytes 56..end   contiguous packed Pubkey[] (32 B each)
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t   table_pubkey[32];   // copy of caller-supplied address
    uint8_t (*addresses)[32];     // heap-allocated, .count entries
    size_t    count;              // 0..256
} jup_alt_table_t;

typedef struct {
    jup_alt_table_t *tables;      // heap-allocated, .table_count entries
    size_t           table_count;
} jup_alt_set_t;

// Fetch every ALT in `table_pubkeys[n]` via getAccountInfo and decode the
// addresses. Returns true on success; on success the output owns heap
// arrays that the caller must release with jup_alt_set_free. On any
// per-table failure the partial state is freed and false is returned.
bool jup_alt_set_fetch(const uint8_t (*table_pubkeys)[32], size_t n,
                       jup_alt_set_t *out);

// Idempotent free. Zeroes the struct.
void jup_alt_set_free(jup_alt_set_t *s);

// Diagnostic — runs the parser AND fetches each referenced ALT, returning
// a JSON summary with per-ALT entry counts (and the first-address base58
// for each, so the host can spot-check which tables were hit).
bool swap_alts_for_test(const char *from_sym, const char *to_sym,
                        double amount_ui, uint16_t slippage_bps,
                        char *out_json, size_t cap);

// Diagnostic — runs the FULL Jupiter /swap-instructions orchestration
// (parse → wrap each ix in vault_execute → fetch ALTs → compose v0 tx
// → sign with the device key) end-to-end and writes the base64 signed
// VersionedTransaction into `out_b64`. Does NOT broadcast — the host
// can hand it to `solana confirm` / `simulateTransaction` to validate
// that the pipeline produced a runtime-acceptable tx before committing
// to a real swap. Used by the host harness only.
bool swap_v2_build_for_test(const char *from_sym, const char *to_sym,
                            double amount_ui, uint16_t slippage_bps,
                            char *out_b64, size_t cap);

#ifdef __cplusplus
}
#endif
