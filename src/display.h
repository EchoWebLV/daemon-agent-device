#pragma once
// ---------------------------------------------------------------------------
//  ILI9342C 320x240 landscape over SPI3 — ESP32-S3-BOX-3B.
//
//  Thin wrapper around the espressif/esp_lcd_ili9341 managed component
//  (it covers both 9341 and 9342C). Initialises the SPI bus, panel IO and
//  panel, draws a navy clear as a smoke test, and exposes the panel handle
//  so esp_lvgl_port can draw into it. Backlight is owned by devcfg.c via
//  LEDC PWM and brought up there, not here.
// ---------------------------------------------------------------------------
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240

// Bring up SPI bus + ILI9342C panel. Safe to call once at boot. MUST run
// before touch_init() — the touch IC's reset is level-shifted from
// LCD_RST, so a late panel reset would yank the touch IC out from under
// LVGL's pointer indev.
esp_err_t display_init(void);

// Panel handle (NULL before display_init returns ESP_OK).
esp_lcd_panel_handle_t display_panel(void);

// Panel IO handle. esp_lvgl_port's display config takes both handles —
// panel for the pixel draw path and IO for sending command/param bytes.
esp_lcd_panel_io_handle_t display_io(void);

#ifdef __cplusplus
}
#endif
