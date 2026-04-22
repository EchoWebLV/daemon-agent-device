// ---------------------------------------------------------------------------
//  CST328 touch → LVGL binding. See touch.h for the interface and why
//  this is a thin wrapper over the managed esp_lcd_touch_cst328 driver.
// ---------------------------------------------------------------------------
#include "touch.h"
#include "display.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst328.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "touch";

// ---- Pin + bus config (Waveshare 2.8") --------------------------------------
// Matches the Arduino build's src/touch.cpp exactly so we're not chasing
// a pin-map regression on top of a framework change.
#define PIN_SDA    1
#define PIN_SCL    3
#define PIN_INT    4
#define PIN_RST    2
#define I2C_PORT   I2C_NUM_0
#define I2C_HZ     400000

static esp_lcd_touch_handle_t s_touch = NULL;

esp_err_t touch_init(void) {
    // ---- I2C master bus (new-style ESP-IDF 5.x API) -------------------------
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = I2C_PORT,
        .sda_io_num                   = PIN_SDA,
        .scl_io_num                   = PIN_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");

    // ---- Panel IO for CST328 over I2C ---------------------------------------
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST328_CONFIG();
    io_cfg.scl_speed_hz = I2C_HZ;   // macro defaults to 100 kHz; we run 400 kHz.
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bus, &io_cfg, &tp_io),
        TAG, "tp io");

    // ---- CST328 driver ------------------------------------------------------
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max          = DISPLAY_WIDTH,
        .y_max          = DISPLAY_HEIGHT,
        .rst_gpio_num   = PIN_RST,
        .int_gpio_num   = PIN_INT,
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
        esp_lcd_touch_new_i2c_cst328(tp_io, &tp_cfg, &s_touch),
        TAG, "cst328 new");

    // ---- Bind to LVGL as a pointer input device ----------------------------
    const lvgl_port_touch_cfg_t lvgl_tp = {
        .disp   = lv_display_get_default(),
        .handle = s_touch,
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&lvgl_tp), ESP_FAIL,
                        TAG, "lvgl_port_add_touch");

    ESP_LOGI(TAG, "CST328 bound to LVGL (I2C %d kHz, INT=%d, RST=%d)",
             I2C_HZ / 1000, PIN_INT, PIN_RST);
    return ESP_OK;
}
