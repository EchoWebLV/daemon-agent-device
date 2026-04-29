// ---------------------------------------------------------------------------
//  touch.c — GT911 capacitive-touch bring-up for the ESP32-S3-BOX-3.
//
//  This device is a BOX-3 (with GT911 touch), not a BOX-3B (TT21100). The
//  distinction was discovered at first bring-up via I2C scan once the
//  LCD/touch reset polarity bug was fixed (see display.c — BOX-3 RST is
//  active HIGH, not the default active LOW).
//
//  Uses the espressif/esp_lcd_touch_gt911 driver registered against the
//  generic esp_lcd_touch abstraction, then bound to LVGL via esp_lvgl_port
//  as a pointer indev.
//
//  I2C bus comes from bus_i2c() (see bus.c) so we share I2C_NUM_0 with the
//  ES8311 + ES7210 audio codecs.
//
//  Reset line: GT911 has no dedicated reset GPIO on the BOX-3 — its reset
//  is level-shifted from LCD_RST (GPIO48). display_init() is the one that
//  drives that line; as long as display_init() runs first (see app_main.c),
//  the touch IC sees exactly one reset pulse during boot.
// ---------------------------------------------------------------------------
#include "touch.h"
#include "display.h"
#include "board.h"
#include "bus.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "touch";

static esp_lcd_touch_handle_t s_touch = NULL;

// Scan the shared I2C bus and log every responding 7-bit address. Helps
// distinguish BOX-3 variants by which touch IC responds (GT911 @ 0x5D
// or 0x14, TT21100 @ 0x24, FT5x06 @ 0x38). Probe timeout 100 ms — slow
// devices that need ack-stretching get caught.
static void i2c_scan(i2c_master_bus_handle_t bus, const char *label) {
    char hits[256] = {0};
    size_t off = 0;
    int found = 0;
    for (int addr = 1; addr < 0x78; ++addr) {
        if (i2c_master_probe(bus, addr, 100) == ESP_OK) {
            off += snprintf(hits + off, sizeof(hits) - off, "0x%02x ", addr);
            ++found;
        }
    }
    if (found) {
        ESP_LOGI(TAG, "I2C scan (%s): %d device(s) responding: %s",
                 label, found, hits);
    } else {
        ESP_LOGW(TAG, "I2C scan (%s): no devices responding", label);
    }
}

esp_err_t touch_init(void) {
    // Pre-touch settle. The shared LCD/touch reset was pulsed during
    // display_init() ~250 ms ago, but for diagnosis we add an extra long
    // wait so we can rule out "IC not ready yet."
    vTaskDelay(pdMS_TO_TICKS(500));

    // Read INT pin — if the touch IC is alive and idle, INT should be HIGH.
    // If it's stuck low, the IC may be in a fault state or held in reset.
    gpio_config_t int_io = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_PIN_INT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_io);
    int int_level = gpio_get_level(BOARD_TOUCH_PIN_INT);
    ESP_LOGI(TAG, "TOUCH_INT (GPIO%d) idle level: %d (1=high, healthy idle for most ICs)",
             BOARD_TOUCH_PIN_INT, int_level);

    i2c_scan(bus_i2c(), "post-500ms-settle");

    // Probe the three known touch addresses explicitly with a long timeout
    // so ack-stretching ICs don't get missed.
    i2c_master_bus_handle_t bus = bus_i2c();
    int found_gt911_a = (i2c_master_probe(bus, 0x5D, 200) == ESP_OK);
    int found_gt911_b = (i2c_master_probe(bus, 0x14, 200) == ESP_OK);
    int found_tt21100 = (i2c_master_probe(bus, 0x24, 200) == ESP_OK);
    ESP_LOGI(TAG, "explicit touch probes: GT911@0x5D=%d  GT911@0x14=%d  TT21100@0x24=%d",
             found_gt911_a, found_gt911_b, found_tt21100);

    // ---- Panel IO for GT911 over the shared I2C bus ------------------------
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = BOARD_I2C_HZ;   // macro defaults to 400 kHz; matches our bus.
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bus_i2c(), &io_cfg, &tp_io),
        TAG, "tp io");

    // ---- GT911 driver ------------------------------------------------------
    // Coordinate maxes match the panel's landscape orientation — display.c
    // configured the LCD as 320x240, so touch reports 0..319 X / 0..239 Y.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max          = DISPLAY_WIDTH,
        .y_max          = DISPLAY_HEIGHT,
        .rst_gpio_num   = -1,                    // shared LCD_RST, driven by display_init
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
    esp_err_t terr = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch);
    if (terr != ESP_OK) {
        ESP_LOGW(TAG, "gt911 new failed (%s) — touch disabled for this boot",
                      esp_err_to_name(terr));
        return ESP_OK;
    }

    // ---- Bind to LVGL as a pointer input device ----------------------------
    const lvgl_port_touch_cfg_t lvgl_tp = {
        .disp   = lv_display_get_default(),
        .handle = s_touch,
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&lvgl_tp), ESP_FAIL,
                        TAG, "lvgl_port_add_touch");

    ESP_LOGI(TAG, "GT911 bound to LVGL (I2C %d kHz, INT=%d, RST=shared LCD_RST)",
             BOARD_I2C_HZ / 1000, BOARD_TOUCH_PIN_INT);
    return ESP_OK;
}

esp_lcd_touch_handle_t touch_handle(void) {
    return s_touch;
}
