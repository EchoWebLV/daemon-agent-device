#pragma once
// ---------------------------------------------------------------------------
//  ST7789 240x320 over SPI2 — Waveshare ESP32-S3-Touch-LCD-2.8.
//
//  Thin wrapper around esp_lcd's built-in ST7789 driver. Initialises the
//  SPI bus, panel IO and panel, turns the backlight on and exposes the
//  panel handle so the LVGL port (next step) can draw into it.
// ---------------------------------------------------------------------------
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 320

// Bring up SPI bus + ST7789 + backlight. Safe to call once at boot.
esp_err_t display_init(void);

// Panel handle (NULL before display_init returns ESP_OK).
esp_lcd_panel_handle_t display_panel(void);

// Panel IO handle. esp_lvgl_port's display config takes both handles —
// panel for the pixel draw path and IO for sending command/param bytes.
esp_lcd_panel_io_handle_t display_io(void);

#ifdef __cplusplus
}
#endif
