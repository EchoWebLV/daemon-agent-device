// ---------------------------------------------------------------------------
//  buttons.h — front-face hardware button → push-to-talk.
//
//  The ESP32-S3-BOX-3 has a small button in the bezel below the LCD.
//  It's not wired to a GPIO; it lives behind the GT911 touch IC's button
//  register, surfaced by esp_lcd_touch_get_button_state(). This module
//  registers an iot_button instance against that bit and routes its
//  press/release events to the existing PTT handlers in creature_screen.c.
//
//  Why we bypass the BSP's bsp_iot_button_create(): that helper creates
//  its own internal esp_lcd_touch handle if one isn't cached in the BSP,
//  and we already own a handle in touch.c. Two handles on the same
//  GT911 over the shared I2C bus would race. Reading from our existing
//  handle via touch_handle() keeps the I2C path single-owner.
//
//  Depends on touch_init() having succeeded; if it didn't, buttons_init()
//  is a no-op (logs a warning) and the boot continues so an unrelated
//  failure doesn't take the whole device down.
// ---------------------------------------------------------------------------
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t buttons_init(void);

#ifdef __cplusplus
}
#endif
