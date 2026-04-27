// ---------------------------------------------------------------------------
//  touch.c — TT21100 capacitive-touch bring-up for the ESP32-S3-BOX-3B.
//
//  Uses the espressif/esp_lcd_touch_tt21100 driver registered against the
//  generic esp_lcd_touch abstraction, then bound to LVGL via esp_lvgl_port
//  as a pointer indev. Same shape as the previous CST328 implementation —
//  only the IC and pin source change.
//
//  I2C bus comes from bus_i2c() (see bus.c) so we share I2C_NUM_0 with the
//  audio codec when M3 lands.
//
//  Reset line: TT21100 has no dedicated reset GPIO on the BOX-3B — its
//  reset is level-shifted from LCD_RST (GPIO48). board.h aliases
//  BOARD_TOUCH_PIN_RST to BOARD_LCD_PIN_RST for documentation, but
//  display_init() is the one that actually drives the line. As long as
//  display_init() runs first (it does, see app_main.c), the touch IC sees
//  exactly one reset pulse during boot.
// ---------------------------------------------------------------------------
#include "touch.h"
#include "display.h"
#include "board.h"
#include "bus.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_tt21100.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "touch";

static esp_lcd_touch_handle_t s_touch = NULL;

esp_err_t touch_init(void) {
    // ---- Panel IO for TT21100 over the shared I2C bus ----------------------
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_TT21100_CONFIG();
    io_cfg.scl_speed_hz = BOARD_I2C_HZ;   // macro defaults to 100 kHz; we run 400 kHz.
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bus_i2c(), &io_cfg, &tp_io),
        TAG, "tp io");

    // ---- TT21100 driver -----------------------------------------------------
    // Coordinate maxes match the panel's landscape orientation — display.c
    // configured the LCD as 320x240, so touch reports 0..319 X / 0..239 Y.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max          = DISPLAY_WIDTH,
        .y_max          = DISPLAY_HEIGHT,
        .rst_gpio_num   = -1,                    // no dedicated reset; see header
        .int_gpio_num   = BOARD_TOUCH_PIN_INT,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_tt21100(tp_io, &tp_cfg, &s_touch),
        TAG, "tt21100 new");

    // ---- Bind to LVGL as a pointer input device ----------------------------
    const lvgl_port_touch_cfg_t lvgl_tp = {
        .disp   = lv_display_get_default(),
        .handle = s_touch,
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&lvgl_tp), ESP_FAIL,
                        TAG, "lvgl_port_add_touch");

    ESP_LOGI(TAG, "TT21100 bound to LVGL (I2C %d kHz, INT=%d, RST=shared LCD_RST)",
             BOARD_I2C_HZ / 1000, BOARD_TOUCH_PIN_INT);
    return ESP_OK;
}
