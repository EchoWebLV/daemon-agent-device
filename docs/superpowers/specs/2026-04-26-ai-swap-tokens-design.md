# AI-driven token swaps (SOL / USDC / held SPLs)

## Goal

Let the Daemon's LLM (and the user, via chat) execute on-device Solana swaps
between SOL, USDC, and the SPL tokens already held in the wallet. Every swap
is gated by a **hold-to-confirm 3-second** approval on the device, with the
trust-essential figures shown on screen.

## Trust model

- All signing happens on-device with the existing wallet key. No third-party
  custody, no relayers, no x402 service involvement on the swap path.
- The user must physically hold the touchscreen for 3 seconds during an
  approval window that displays the actual figures the LLM proposed.
- The LLM cannot autonomously swap. It can only *propose* a swap, which
  surfaces an approval screen the user must explicitly authorize.

## Scope (V1)

**In:** SOL ↔ USDC, plus any SPL token already in the wallet's holdings list
to/from SOL or USDC.

**Out (V1):** swapping into a brand-new mint the wallet doesn't yet hold,
arbitrary mint-to-mint pairs that don't involve SOL/USDC, custom DEXes
(Jupiter only), limit orders, DCA, anything that requires a non-versioned
transaction or signature aggregation.

## Architecture

### New components

**`src/swap.c` / `src/swap.h`** — Jupiter integration + orchestration.

```c
typedef enum {
    SWAP_OK,
    SWAP_ERR_QUOTE,
    SWAP_ERR_INSUFFICIENT,
    SWAP_ERR_CANCELLED,
    SWAP_ERR_IN_PROGRESS,
    SWAP_ERR_BUILD,
    SWAP_ERR_SUBMIT,
    SWAP_ERR_UNCONFIRMED,
} swap_status_t;

typedef struct {
    swap_status_t status;
    char          txid[96];      // base58, "" on failure
    double        amount_in;     // UI units
    double        amount_out;    // UI units, 0 on failure
    char          from_sym[12];
    char          to_sym[12];
    char          error_msg[64]; // short, LLM-readable
} swap_result_t;

typedef void (*swap_done_cb_t)(const swap_result_t *r, void *user);

// Synchronous from the caller's POV — the calling task blocks on a semaphore
// until the approval window resolves and the swap completes (or fails).
// `slippage_bps` of 0 → use default (50 for SOL/USDC, 100 for SPLs).
// Out-of-range (<10 or >500) is clamped to the default.
bool swap_request(const char *from_sym,
                  const char *to_sym,
                  double      amount_ui,
                  uint16_t    slippage_bps,
                  swap_result_t *out);
```

**`src/swap_screen.c` / `src/swap_screen.h`** — LVGL approval modal.

```c
typedef struct {
    char     from_sym[12];
    char     to_sym[12];
    double   amount_in;        // UI units
    double   amount_out;       // expected UI units
    double   min_out;          // amount_out * (1 - slip)
    uint16_t slippage_bps;
    double   fee_sol;          // total network + ATA-rent estimate
} swap_screen_args_t;

typedef enum { SWAP_UI_CONFIRM, SWAP_UI_CANCEL_RELEASE,
               SWAP_UI_CANCEL_SWIPE, SWAP_UI_CANCEL_TIMEOUT } swap_ui_result_t;

// Opens the modal via lv_async_call. Caller blocks on `done_sem` until
// the user confirms or cancels (via release-too-soon, any swipe, or 30s).
void swap_screen_open(const swap_screen_args_t *args,
                      SemaphoreHandle_t  done_sem,
                      swap_ui_result_t  *out_result);
```

**Synthetic tool in `src/ai.c`** — `swap_tokens` registered alongside x402
services in `attach_tools()`. `execute_tool()` checks the name first; if it
matches, it parses args (`from`, `to`, `amount`, optional `max_slippage_bps`)
and calls `swap_request()` instead of `x402_call()`. Tool-response shape:

```json
// success
{"ok": true, "txid": "5Vfy...", "received": 0.0431, "from": "USDC", "to": "SOL"}
// failure
{"ok": false, "error": "cancelled" | "insufficient_balance" | ...}
```

The tool's OpenAI schema is added to the tools array unconditionally
(no NVS toggle — it's a built-in capability). Description tells the LLM
that the user must approve via hardware hold-to-confirm.

### Reused components

- `wallet_sign()` — Ed25519 over the v0 tx message bytes
- `wallet_pubkey_bytes()` — for substituting the right signature slot
- `wallet_rpc_url()` — for `sendTransaction` + `getSignatureStatuses`
- `wallet_request_refresh()` — fires `wallet_incoming_cb` after confirm,
  which drives the creature reaction (existing path)
- `x402.c` HTTP plumbing is **not** used; the swap path uses `esp_http_client`
  directly with the existing TLS + PSRAM mbedtls setup
- LVGL screen-stack + screen transition system

## Data flow (one swap)

```
LLM tool_call swap_tokens(from, to, amount, max_slippage_bps?)
        │
        ▼  (in ai.c execute_tool)
synthetic-tool dispatch → swap_request(...)
        │
        │  blocks on result semaphore
        ▼
swap.c:
 1. balance check against wallet's last refresh
       fail → SWAP_ERR_INSUFFICIENT
 2. resolve from/to symbols → mint addresses
       (SOL = So11... wrapped sentinel for Jupiter; USDC = canonical mint;
        SPL = look up in wallet token list)
 3. GET https://quote-api.jup.ag/v6/quote?inputMint=...&outputMint=...
        &amount=<atomic>&slippageBps=<n>&onlyDirectRoutes=false
       fail → SWAP_ERR_QUOTE
 4. lv_async_call → swap_screen_open(args)  with from/to/min_out/slip/fee
 5. wait on done_sem (30s timeout enforced inside the screen)
       cancel of any flavor → SWAP_ERR_CANCELLED
 6. POST https://quote-api.jup.ag/v6/swap
       body: { quoteResponse, userPublicKey, wrapAndUnwrapSol: true,
               dynamicComputeUnitLimit: true, prioritizationFeeLamports: 1 }
       fail → SWAP_ERR_BUILD
 7. base64-decode `swapTransaction`. Parse the v0 wire format header to
    locate the message-to-sign region. (See "Tx signing" below.)
 8. Splice wallet signature into our slot
 9. RPC sendTransaction (base64 encoding=base64, skipPreflight=false)
       fail → SWAP_ERR_SUBMIT
10. RPC getSignatureStatuses, poll every 800ms up to 30s
       not landed → SWAP_ERR_UNCONFIRMED (txid still returned)
11. wallet_request_refresh()  → wallet_incoming_cb fires after balance
    snapshot updates → creature speaks via existing reaction path
12. signal `swap_request` caller via semaphore → returns to ai.c →
    tool-response sent back to the LLM
```

## Behavior nuances

### Slippage clamping

| Pair               | Default | Min   | Max   |
|--------------------|---------|-------|-------|
| SOL ↔ USDC         | 50 bps  | 10    | 500   |
| Anything involving SPL | 100 bps | 10 | 500 |

LLM-supplied `max_slippage_bps` outside `[10, 500]` falls back to the default
silently. Inside the range, LLM's value wins. Whatever value is used appears
on the approval screen.

### Quote freshness

The quote in steps 3–5 is what the user sees on screen. The actually-built
swap tx in step 6 is fetched fresh on confirm. Routing may shift between
those two calls; slippage cap guarantees the user receives at least the
`min_out` displayed.

### Concurrency

A single global `s_swap_in_progress` flag in swap.c. While set, any new
`swap_request` returns `SWAP_ERR_IN_PROGRESS` immediately with no UI.
The flag is set when the screen opens and cleared after step 11 (or any
earlier failure path).

### Cancellation

The approval screen returns CANCEL on:
- Touch released before 3s of continuous hold
- Any swipe gesture (existing global swipe handler is suppressed while the
  modal is open and the modal handles its own touch)
- 30s with no touch at all

All three collapse to `SWAP_ERR_CANCELLED` for the caller; the screen does
not distinguish. (Internal telemetry in logs only.)

### ATA creation

Jupiter's `/swap` response handles missing destination ATAs via the bundled
`createIdempotent` instruction. The ~0.002 SOL rent is part of the fee
displayed on the approval screen. Any SOL the wallet doesn't have at submit
time → `SWAP_ERR_SUBMIT` (RPC rejects). No special-case logic.

### Fee figure on the approval screen

Computed at quote time as:

```
fee_sol = base_fee  + priority_fee + ata_rent
        = 0.000005  + 0.000000001  + (0.00203928 if dest ATA missing else 0)
```

Base fee = 5000 lamports per required signature (one signer = ours; fee
payer slot is filled by Jupiter's relayer in some routes but commonly
`userPublicKey` covers it — we always show the worst case of ~5000 lamports).
Priority fee = the `prioritizationFeeLamports: 1` we request. ATA rent =
0.00203928 SOL (rent-exempt minimum for a token account, fixed) added only
if the destination ATA isn't in the wallet's cached token list.

If Jupiter returns a `prioritizationFeeLamports` larger than what we asked
for (it sometimes inflates for landing reliability), we use the returned
value instead.

### Tx signing

Jupiter v6 returns a base64-encoded *signed* v0 versioned transaction with
`numRequiredSignatures` slots; ours is unsigned (zeros). We do **not** parse
the address lookup tables — we only need:

1. Decode base64 → wire bytes
2. Read `numRequiredSignatures` (first byte) and skip that many 64-byte
   signature slots → `messageStart`
3. The remaining bytes from `messageStart` to end-of-buffer are the
   message-to-sign
4. Find which signature slot is ours by matching `wallet_pubkey_bytes()`
   against the static-account-keys section of the message header
5. Sign the message bytes via `wallet_sign()`, write the 64-byte sig into
   the located slot
6. Re-encode the whole buffer as base64 for `sendTransaction`

This avoids implementing ALT resolution at all; the static-account section
contains our pubkey directly because we're a signer.

## Memory budget

| Item                                | Size           |
|-------------------------------------|----------------|
| Jupiter `/quote` response buffer    | 4 KB           |
| Jupiter `/swap` response buffer     | 8 KB (cap; reject larger) |
| Decoded swap tx bytes               | ~3 KB peak     |
| swap.c module state                 | < 1 KB         |
| swap_screen.c LVGL objects          | ~3 KB (alive only while shown) |
| New flash (compiled)                | ~25 KB         |

Reuses existing TLS/mbedtls (PSRAM), HTTP client, cJSON, LVGL infra. No new
persistent allocations. Peak swap-time heap usage is comparable to a single
LLM round-trip.

## Error handling

All failure modes resolve to a single tool-response shape so the LLM can
talk about the result, plus the creature speaks specific cases via the
existing reaction path.

| Code                  | Tool-response error string  | Creature speaks? |
|-----------------------|------------------------------|------------------|
| `SWAP_OK`             | (no error)                   | yes (incoming)   |
| `SWAP_ERR_QUOTE`      | `quote_failed`               | no (LLM does)    |
| `SWAP_ERR_INSUFFICIENT` | `insufficient_balance`     | no (LLM does)    |
| `SWAP_ERR_CANCELLED`  | `cancelled`                  | yes              |
| `SWAP_ERR_IN_PROGRESS`| `swap_in_progress`           | no               |
| `SWAP_ERR_BUILD`      | `build_failed`               | no (LLM does)    |
| `SWAP_ERR_SUBMIT`     | `submit_failed`              | yes              |
| `SWAP_ERR_UNCONFIRMED`| `unconfirmed`                | no (LLM does)    |

## Approval screen layout

240×280 panel, pixel-art aesthetic to match the rest of the device:

```
┌────────────────────────┐
│        SWAP            │
│                        │
│    5.00 USDC           │
│       ↓                │
│    0.0431 SOL          │
│                        │
│  min  0.0429 SOL       │
│  slip 0.5%             │
│  fee  0.00001 SOL      │
│                        │
│      ╭──╮              │
│      │ 3│  hold        │
│      ╰──╯              │
└────────────────────────┘
```

The hold indicator is a circular arc filling clockwise as the user holds.
Any release before fill completes returns CANCEL. Any swipe returns CANCEL.

## Testing

- **Unit-ish (host or device fixture):** feed a captured Jupiter `/swap`
  response into the v0 tx signer; assert the spliced signature verifies
  via Ed25519 against the message bytes.
- **Manual smoke:** real device, "swap 0.1 USDC to SOL" through chat →
  approval shown with sane figures → hold 3s → wallet refresh shows the
  bump → creature reacts. Repeat with cancel-by-release, cancel-by-swipe.
- **Test-harness verb:** add `swap_dry_run` to the Phase-2 harness. Runs
  steps 1–4 (quote + open approval) without submitting, returns the
  computed approval-screen args as a JSON blob.

## Out of scope (deferred)

- Swapping into a mint not in the wallet's holdings list. Requires
  trust UX for "do you trust this mint?" — explicitly punted.
- Configurable defaults via the phone UI. Defaults are hardcoded in V1.
- Multi-signature flows.
- Mainnet vs. devnet toggle (mainnet only, like the rest of the firmware).
- Transaction history / swap log UI.
