#pragma once
// ---------------------------------------------------------------------------
//  LVGL bring-up — proves the display is driveable from the real UI stack
//  we'll use for the rest of the app.
//
//  Depends on display_init() having already returned ESP_OK. Once this
//  returns, LVGL is running on its own task (courtesy of esp_lvgl_port)
//  and drawing at ~30 FPS into partial buffers in internal RAM.
// ---------------------------------------------------------------------------
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_init(void);

#ifdef __cplusplus
}
#endif
