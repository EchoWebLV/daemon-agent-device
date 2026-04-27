// ---------------------------------------------------------------------------
//  bus.c — see bus.h for the why.
// ---------------------------------------------------------------------------
#include "bus.h"
#include "board.h"

#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "bus";

static i2c_master_bus_handle_t s_bus = NULL;

i2c_master_bus_handle_t bus_i2c(void) {
    if (s_bus) return s_bus;

    const i2c_master_bus_config_t cfg = {
        .i2c_port                     = BOARD_I2C_PORT,
        .sda_io_num                   = BOARD_I2C_PIN_SDA,
        .scl_io_num                   = BOARD_I2C_PIN_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
    // The new ESP-IDF 5.x i2c_master API allocates the bus speed-agnostic
    // and lets each device pick its own SCL frequency at add-device time
    // (see touch.c's `io_cfg.scl_speed_hz = BOARD_I2C_HZ`). So this log
    // intentionally does NOT advertise a bus-wide kHz number — it would
    // mislead any future codec/sensor driver into trusting the bus is
    // already running at 400 kHz when in fact it isn't until they ask.
    ESP_LOGI(TAG, "I2C%d allocated (SDA=%d SCL=%d); per-device speed set on add",
             BOARD_I2C_PORT,
             BOARD_I2C_PIN_SDA, BOARD_I2C_PIN_SCL);
    return s_bus;
}
