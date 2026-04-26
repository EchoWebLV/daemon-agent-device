// ---------------------------------------------------------------------------
//  Approval modal for on-device swaps. Opened from swap.c on the LVGL
//  thread via lv_async_call; the calling task waits on `done_sem`.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     from_sym[12];
    char     to_sym[12];
    double   amount_in;        // UI units
    double   amount_out;       // expected UI units
    double   min_out;          // amount_out * (1 - slippage)
    uint16_t slippage_bps;
    double   fee_sol;          // base + priority + ATA-rent if any
} swap_screen_args_t;

typedef enum {
    SWAP_UI_CONFIRM,
    SWAP_UI_CANCEL_RELEASE,
    SWAP_UI_CANCEL_SWIPE,
    SWAP_UI_CANCEL_TIMEOUT,
} swap_ui_result_t;

// Open the modal. Caller blocks on `done_sem`; on signal, `*out_result`
// holds the resolution. The modal closes itself before signalling.
// Safe to call from any task — internally schedules via lv_async_call.
void swap_screen_open(const swap_screen_args_t *args,
                      SemaphoreHandle_t        done_sem,
                      swap_ui_result_t        *out_result);

#ifdef __cplusplus
}
#endif
