// ---------------------------------------------------------------------------
//  swap_screen.c — see swap_screen.h. LVGL implementation in Task 5.
// ---------------------------------------------------------------------------
#include "swap_screen.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "swap_screen";

void swap_screen_open(const swap_screen_args_t *args,
                      SemaphoreHandle_t        done_sem,
                      swap_ui_result_t        *out_result) {
    (void)args;
    if (out_result) *out_result = SWAP_UI_CANCEL_TIMEOUT;
    ESP_LOGW(TAG, "swap_screen_open stub — auto-cancelling");
    if (done_sem) xSemaphoreGive(done_sem);
}
