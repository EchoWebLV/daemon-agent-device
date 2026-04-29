#pragma once
// ---------------------------------------------------------------------------
//  GT911 capacitive-touch bring-up for the ESP32-S3-BOX-3.
//
//  Sources the shared I2C master bus from bus_i2c(), creates an esp_lcd_touch
//  handle over the GT911, and binds it to LVGL as a pointer input device
//  via esp_lvgl_port. After touch_init() returns ESP_OK, LVGL widgets
//  receive click / press / drag events the usual way — no further plumbing
//  needed.
//
//  Depends on:
//    - ui_init() having already created the default lv_display.
//    - display_init() having run first (BOX-3's touch reset is level-shifted
//      from LCD_RST, so the touch IC needs the panel reset before we talk
//      I2C to it). See touch.c for the full rationale.
// ---------------------------------------------------------------------------
#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t touch_init(void);

// Returns the underlying esp_lcd_touch handle once touch_init() has succeeded.
// Used by buttons.c to read the GT911's home-button bit via
// esp_lcd_touch_get_button_state(). NULL before touch_init() or if init failed.
esp_lcd_touch_handle_t touch_handle(void);

#ifdef __cplusplus
}
#endif
