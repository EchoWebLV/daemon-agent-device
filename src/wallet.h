// ---------------------------------------------------------------------------
//  Daemon's Solana wallet — holds the public address, polls Helius (or the
//  public RPC) for balances, signs Ed25519 payloads for x402, and exposes a
//  short human-readable summary for the AI prompt.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 44 chars is the longest a mainnet Solana address can be; +1 for NUL.
#define WALLET_MINT_MAX   45
#define WALLET_SYM_MAX    12
#define WALLET_MAX_TOKENS 10

typedef struct {
    char    mint[WALLET_MINT_MAX];     // base58 mint address, NUL-terminated
    char    symbol[WALLET_SYM_MAX];    // "" when we don't have it cached
    double  amount;                    // UI amount (already divided by 10^dec)
    uint8_t decimals;
} token_holding_t;

// Decode SOLANA_KEY from secrets.h, derive and store the 32-byte public
// address. Returns false if the key is missing or malformed.
bool wallet_begin(void);

// Base58 pubkey as a NUL-terminated string. "" if wallet_begin() failed.
const char *wallet_pubkey(void);

// 32-byte pubkey, or NULL if the wallet wasn't initialised.
const uint8_t *wallet_pubkey_bytes(void);

// Recovery-authority pubkey decoded from secrets.h's OWNER_PUBKEY. NULL if
// OWNER_PUBKEY is unset, malformed, or still the placeholder. The on-chain
// agent_root PDA is derived from this pubkey, not from the device key.
const uint8_t *wallet_owner_pubkey_bytes(void);
const char    *wallet_owner_pubkey(void);

// True when a 64-byte seed is loaded and the derived pubkey matches.
bool wallet_can_sign(void);

// Ed25519-signs `data` into `sig_out`. False if !wallet_can_sign().
bool wallet_sign(const uint8_t *data, size_t len, uint8_t sig_out[64]);

// Writes the Solana JSON-RPC endpoint into `out` (Helius when HELIUS_API_KEY
// is configured, otherwise the public mainnet endpoint). Pure function of
// secrets.h — safe before wallet_begin().
void wallet_rpc_url(char *out, size_t cap);

// Blocking two-call RPC round-trip (getBalance + getTokenAccountsByOwner).
// Safe to call from the background task.
void wallet_refresh(void);

// Kick the background refresh task; returns immediately.
void wallet_request_refresh(void);

// Most recent balance snapshot. 0.0 before the first refresh.
double wallet_sol_balance(void);

// USDC convenience — sums all SPL holdings whose mint is USDC's.
double      wallet_usdc_amount(void);
const char *wallet_usdc_ata(void);       // "" if no ATA seen yet

// Time since the last successful refresh completed, in milliseconds.
// UINT32_MAX before the first refresh.
uint32_t wallet_last_refresh_age_ms(void);

// Exposes the cached token list. `*out` points into module-local storage
// and is valid until the next wallet_refresh(). `*count` is 0..WALLET_MAX_TOKENS.
void wallet_tokens(const token_holding_t **out, size_t *count);

// Short form for the status bar. "USDC 12.34" / "USDC ---".
void wallet_usdc_display_string(char *out, size_t cap);

// Multi-line summary suitable for injection into Gemini's system prompt.
// Writes up to cap-1 bytes plus NUL. If `sol_usd_price` > 0 the SOL line
// also gets a "(~$X.XX)" suffix.
void wallet_context(char *out, size_t cap, double sol_usd_price);

// Fires once per positive balance delta whose amount crosses an internal
// threshold (> 0.01 SOL, > 0.1 USDC, > 0.1 for other SPL tokens). `symbol`
// is a short ticker ("SOL" / "USDC" / known-mint symbol / "tokens" when
// unknown). Never fires on the first refresh after boot — the starting
// balance would otherwise announce itself. Runs on the wallet task, so
// blocking work (network calls, TTS kick-off) inside the callback is OK.
typedef void (*wallet_incoming_cb_t)(const char *symbol, double amount);
void wallet_set_incoming_cb(wallet_incoming_cb_t cb);

#ifdef __cplusplus
}
#endif
