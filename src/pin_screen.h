// ---------------------------------------------------------------------------
//  PIN keypad screen.
//
//  Modal LVGL screen that takes a numeric PIN. Used at boot to gate the
//  wallet's seed unlock (pin_unlock from pin.h), and at first-time setup
//  to capture a new PIN.
//
//  Use pattern:
//      pin_screen_open(4, 6, on_done, user_ctx);
//      // user types digits ↦ on backspace ↦ on OK fires the callback
//      // callback gets the entered string + a result code
//
//  The screen does no crypto itself. Caller does the unlock attempt and
//  may call pin_screen_set_status() with "Wrong PIN, N left" or similar
//  to show the result without dismissing the screen, then
//  pin_screen_clear_input() to let the user try again.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_SCR_OK,         // user pressed OK with a valid-length PIN
    PIN_SCR_CANCEL,     // user pressed cancel (the X button)
} pin_screen_result_t;

// Fired on the LVGL task. `pin` is a NUL-terminated digits string; valid
// only inside the callback (copy if needed). `user` is the pointer passed
// into pin_screen_open.
typedef void (*pin_screen_cb_t)(pin_screen_result_t r, const char *pin, void *user);

// Show the keypad. min_len / max_len bound the entered PIN length (caller
// can clamp further). If a screen is already open this is a no-op.
void pin_screen_open(int min_len, int max_len,
                     pin_screen_cb_t cb, void *user);

// Update the small status line under the keypad. NULL clears it.
void pin_screen_set_status(const char *text);

// Wipe the entered digits without dismissing the screen — use after a
// failed unlock so the user can retry.
void pin_screen_clear_input(void);

// Tear down the screen; safe to call when nothing is open.
void pin_screen_close(void);

#ifdef __cplusplus
}
#endif
