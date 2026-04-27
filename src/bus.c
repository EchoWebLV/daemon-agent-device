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
    ESP_LOGI(TAG, "I2C%d up @ %d kHz (SDA=%d SCL=%d)",
             BOARD_I2C_PORT, BOARD_I2C_HZ / 1000,
             BOARD_I2C_PIN_SDA, BOARD_I2C_PIN_SCL);
    return s_bus;
}
