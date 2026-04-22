#pragma once
// ---------------------------------------------------------------------------
//  CST328 capacitive-touch bring-up for the Waveshare 2.8" panel.
//
//  Brings up I2C, creates an esp_lcd_touch handle over the CST328, and
//  binds it to LVGL as a pointer input device via esp_lvgl_port. After
//  touch_init() returns ESP_OK, LVGL widgets receive click / press / drag
//  events the usual way — no further plumbing needed.
//
//  Depends on ui_init() having already created the default lv_display.
// ---------------------------------------------------------------------------
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t touch_init(void);

#ifdef __cplusplus
}
#endif
