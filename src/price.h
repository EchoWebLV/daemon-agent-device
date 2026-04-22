// ---------------------------------------------------------------------------
//  SOL/USD price ticker. Fetched from CoinGecko (no key required) every
//  ~30 seconds from the main loop. Rendered in the top-right of the status
//  bar and injected into the AI context so Daemon can quote the price.
//
//  price_refresh() is blocking; price_request_refresh() kicks a background
//  FreeRTOS task so the main loop doesn't stall on TLS.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// No-op for now — kept so callers can stay symmetric with wallet_begin().
bool price_begin(void);

// Blocking HTTPS fetch. Updates the cached price on success; leaves the
// previous value untouched on failure.
void price_refresh(void);

// Fire-and-forget: kicks the background task to run price_refresh().
// Safe to call from anywhere, including before Wi-Fi is up (the task just
// no-ops the refresh in that case).
void price_request_refresh(void);

// Most recent SOL/USD value. 0.0 before the first successful fetch.
double price_sol_usd(void);

// Short form for the status bar. Writes a NUL-terminated string into
// `out` (up to cap-1 bytes). Example: "SOL $198.42" / "SOL --".
void price_display_string(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
