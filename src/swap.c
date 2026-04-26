// ---------------------------------------------------------------------------
//  swap.c — see swap.h.
//
//  Pipeline (one swap):
//    1. resolve symbols → mints + decimals
//    2. balance check against wallet snapshot
//    3. Jupiter /quote → outAmount + min_out + verbatim quote-response JSON
//    4. open approval modal, block on confirm/cancel
//    5. Jupiter /swap (sends quote-response back) → unsigned v0 tx
//    6. parse v0 wire format, locate our signature slot, sign + splice
//    7. RPC sendTransaction
//    8. RPC getSignatureStatuses, poll up to 30s
//    9. wallet_request_refresh() → creature speaks via incoming-payment cb
// ---------------------------------------------------------------------------
#include "swap.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "wallet.h"

static const char *TAG = "swap";

// Jupiter accepts wrapped-SOL mint as the sentinel for native SOL whenever
// the request includes wrapAndUnwrapSol=true. We always do, so we always
// pass this for "SOL".
static const char *NATIVE_SOL_MINT = "So11111111111111111111111111111111111111112";
static const char *USDC_MINT       = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

typedef struct {
    char    mint[45];      // base58 mint, NUL-terminated
    char    sym[12];        // canonical symbol ("SOL", "USDC", or wallet ticker)
    uint8_t decimals;
} swap_token_t;

// Resolve a user-supplied symbol to mint + decimals. Case-insensitive.
// "SOL" / "USDC" are hard-coded; anything else is matched against the
// wallet's most recent SPL holdings (first match wins on duplicate symbols).
// Returns false if not resolvable.
static bool resolve_token(const char *sym, swap_token_t *out) {
    if (!sym || !out) return false;
    memset(out, 0, sizeof(*out));

    if (strcasecmp(sym, "SOL") == 0) {
        strlcpy(out->mint, NATIVE_SOL_MINT, sizeof(out->mint));
        strlcpy(out->sym,  "SOL",            sizeof(out->sym));
        out->decimals = 9;
        return true;
    }
    if (strcasecmp(sym, "USDC") == 0) {
        strlcpy(out->mint, USDC_MINT, sizeof(out->mint));
        strlcpy(out->sym,  "USDC",     sizeof(out->sym));
        out->decimals = 6;
        return true;
    }

    const token_holding_t *toks = NULL;
    size_t n = 0;
    wallet_tokens(&toks, &n);
    for (size_t i = 0; i < n; ++i) {
        if (strcasecmp(toks[i].symbol, sym) == 0) {
            strlcpy(out->mint, toks[i].mint,   sizeof(out->mint));
            strlcpy(out->sym,  toks[i].symbol, sizeof(out->sym));
            out->decimals = toks[i].decimals;
            return true;
        }
    }
    return false;
}

bool swap_resolve_for_test(const char *sym, char *out, size_t cap) {
    swap_token_t t;
    if (!resolve_token(sym, &t)) return false;
    snprintf(out, cap, "%s:%u", t.mint, (unsigned)t.decimals);
    return true;
}

bool swap_request(const char *from_sym,
                  const char *to_sym,
                  double      amount_ui,
                  uint16_t    slippage_bps,
                  swap_result_t *out) {
    (void)from_sym; (void)to_sym; (void)amount_ui; (void)slippage_bps;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->status = SWAP_ERR_QUOTE;
    strlcpy(out->error_msg, "not_implemented", sizeof(out->error_msg));
    ESP_LOGW(TAG, "swap_request stub — not implemented yet");
    return false;
}
