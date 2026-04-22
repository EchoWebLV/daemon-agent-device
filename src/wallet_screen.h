// ---------------------------------------------------------------------------
//  Wallet screen — Daemon's Solana holdings at a glance.
//
//  Shows:
//    • truncated public address          (top, tappable copies to clip later)
//    • big SOL balance + USD equivalent
//    • HOLDINGS divider
//    • scrollable list of SPL tokens from wallet_tokens()
//    • footer hint ("swipe →" / "swipe ←")
//
//  The caller drives refresh: this module pulls data from wallet.h on
//  wallet_screen_refresh() and re-renders. The main loop already fires
//  wallet_request_refresh() every 60 s, so calling wallet_screen_refresh()
//  in the same cadence keeps the view current without adding a new timer.
//
//  Thread-safety: same rule as creature_screen.c — setters take the
//  lvgl_port mutex themselves. Refresh also does one read from wallet
//  state atomically while holding the mutex.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

bool wallet_screen_init(void);
lv_obj_t *wallet_screen(void);

// Shared status-bar setters mirror the creature screen's API, so the same
// main-loop pump can update both without a special-case.
void wallet_screen_set_status(const char *s);
void wallet_screen_set_price(const char *s);
void wallet_screen_set_usdc(const char *s);

// Re-read wallet_sol_balance() / wallet_usdc_amount() / wallet_tokens()
// and redraw the list. Safe to call from the main loop.
void wallet_screen_refresh(void);

#ifdef __cplusplus
}
#endif
