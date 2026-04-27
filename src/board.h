// ---------------------------------------------------------------------------
//  board.h — single source of truth for ESP32-S3-BOX-3 GPIO assignments.
//
//  Every other source file references these constants instead of hard-coding
//  GPIO numbers. Keeping pins here means a future board port is one file.
//
//  Pinout was lifted from Espressif's official BSP source on 2026-04-27:
//    espressif/esp-bsp · bsp/esp-box-3/include/bsp/esp-box-3.h (master)
//
//  Two non-obvious facts the implementation must respect:
//
//    1. LCD lives on SPI3, not SPI2. The Waveshare predecessor used SPI2,
//       so any leftover `SPI2_HOST` reference will silently route pixels
//       to the wrong pins.
//
//    2. The GT911 touch IC's reset line has no dedicated GPIO — it's
//       level-shifted from LCD_RST (GPIO48). That means display_init()
//       MUST run before touch_init(); a panel reset pulse after the
//       touch IC is up will yank the touch controller out from under
//       the LVGL pointer indev.
//
//  Audio (ES8311 speaker + ES7210 mic) and the I2C bus pins are owned by
//  the BOX-3 BSP — see managed_components/espressif__esp-box-3/. We don't
//  redeclare them here because the BSP would just override us anyway.
// ---------------------------------------------------------------------------
#pragma once

#include "driver/gpio.h"
#include "driver/spi_common.h"

// === Display: ILI9342C over SPI3 ============================================
#define BOARD_LCD_SPI_HOST          SPI3_HOST
#define BOARD_LCD_PIN_MOSI          GPIO_NUM_6
#define BOARD_LCD_PIN_SCLK          GPIO_NUM_7
#define BOARD_LCD_PIN_CS            GPIO_NUM_5
#define BOARD_LCD_PIN_DC            GPIO_NUM_4
#define BOARD_LCD_PIN_RST           GPIO_NUM_48   // shared with touch RST
#define BOARD_LCD_PIN_BL            GPIO_NUM_47   // owned by devcfg LEDC
#define BOARD_LCD_PIXEL_CLOCK_HZ    (40 * 1000 * 1000)
#define BOARD_LCD_H_RES             320
#define BOARD_LCD_V_RES             240

// === Touch: GT911 ===========================================================
#define BOARD_TOUCH_PIN_INT         GPIO_NUM_3
#define BOARD_I2C_HZ                400000        // GT911 I2C speed
