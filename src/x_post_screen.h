// ---------------------------------------------------------------------------
//  Approval modal for "post this draft to X?". Opened from social_x.c on
//  the LVGL thread via lv_async_call; the calling task waits on `done_sem`.
//
//  Identical control pattern to swap_screen — hold-to-confirm, release/swipe/
//  timeout cancels — so muscle memory carries between the two screens.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char text  [300];   // draft post (X cap is 280; 300 leaves slack)
    char handle[32];    // @handle of the connected account, for "Posting as @x"
} x_post_screen_args_t;

typedef enum {
    X_POST_UI_CONFIRM,
    X_POST_UI_CANCEL_RELEASE,
    X_POST_UI_CANCEL_SWIPE,
    X_POST_UI_CANCEL_TIMEOUT,
} x_post_ui_result_t;

void x_post_screen_open(const x_post_screen_args_t *args,
                        SemaphoreHandle_t          done_sem,
                        x_post_ui_result_t        *out_result);

#ifdef __cplusplus
}
#endif
