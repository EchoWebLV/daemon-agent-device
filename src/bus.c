// ---------------------------------------------------------------------------
//  bus.c — see bus.h.
//
//  Delegates to the esp-box-3 BSP for I2C master setup. The BSP's audio
//  codec init helpers internally call bsp_i2c_init(), so every I2C consumer
//  in the project (touch + both codecs) MUST share the BSP's bus handle to
//  avoid double-init conflicts on I2C_NUM_0. bsp_i2c_init() is idempotent.
// ---------------------------------------------------------------------------
#include "bus.h"

#include "bsp/esp-box-3.h"

#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "bus";

i2c_master_bus_handle_t bus_i2c(void) {
    static bool s_logged_once = false;
    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_master_bus_handle_t h = bsp_i2c_get_handle();
    if (!s_logged_once) {
        s_logged_once = true;
        ESP_LOGI(TAG, "shared I2C bus from BSP (handle=%p)", h);
    }
    return h;
}
