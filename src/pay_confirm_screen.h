// ---------------------------------------------------------------------------
//  pay_confirm_screen.h — hold-to-confirm modal for paid x402 calls.
//
//  Opened from payapi_guard when the payment amount is above the auto-pay
//  threshold but within the confirm cap. The calling task blocks until the
//  user resolves the modal (hold 3 s → OK, any cancel path → DENIED).
// ---------------------------------------------------------------------------
#ifndef PAY_CONFIRM_SCREEN_H
#define PAY_CONFIRM_SCREEN_H

#include <stdint.h>
#include "x402.h"   // for x402_guard_decision_t

#ifdef __cplusplus
extern "C" {
#endif

// Open the hold-to-confirm modal and block the calling task until the
// user resolves it. Returns one of:
//   X402_GUARD_CONFIRM_OK     — held the dial for 3 s
//   X402_GUARD_CONFIRM_DENIED — release before 3 s, [X] tap, 30 s no-touch
//                                timeout, or PTT generation bumped
//
// Safe to call from any non-LVGL task; modal construction is dispatched
// to the LVGL thread via lv_async_call. Caller blocks on a semaphore
// until the resolution callback fires.
x402_guard_decision_t pay_confirm_screen_show_blocking(
    uint64_t    actual_micros,
    const char *description,
    uint32_t    ptt_gen);

#ifdef __cplusplus
}
#endif
#endif  // PAY_CONFIRM_SCREEN_H
