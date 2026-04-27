// ---------------------------------------------------------------------------
//  bus.h — shared I2C master bus for the BOX-3's touch IC + audio codec.
//
//  Why this exists: GT911 (touch) and ES8311/ES7210 (audio codec control)
//  all live on I2C_NUM_0. The new-style ESP-IDF 5.x i2c_master API only lets
//  one caller create the bus; the second i2c_new_master_bus() returns
//  ESP_ERR_INVALID_STATE. So we delegate to bsp_i2c_init() which is itself
//  idempotent, and hand the same handle out to everyone.
// ---------------------------------------------------------------------------
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the shared bus handle, creating it on first call. Panics
// (ESP_ERROR_CHECK) on failure — the device is unusable without I2C, so
// there's no graceful fallback.
i2c_master_bus_handle_t bus_i2c(void);

#ifdef __cplusplus
}
#endif
