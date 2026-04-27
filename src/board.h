// ---------------------------------------------------------------------------
//  board.h — single source of truth for ESP32-S3-BOX-3B GPIO assignments.
//
//  Every other source file references these constants instead of hard-coding
//  GPIO numbers. Keeping pins here means a future board port is one file.
//
//  Pinout was lifted from Espressif's official BSP source on 2026-04-27:
//    espressif/esp-bsp · bsp/esp-box-3/include/bsp/esp-box-3.h (master)
//  The same BSP serves both BOX-3 and BOX-3B; only the touch IC differs.
//
//  Two non-obvious facts the implementation must respect:
//
//    1. LCD lives on SPI3, not SPI2. The Waveshare predecessor used SPI2,
//       so any leftover `SPI2_HOST` reference will silently route pixels
//       to the wrong pins.
//
//    2. The TT21100 touch IC's reset line has no dedicated GPIO — it's
//       level-shifted from LCD_RST (GPIO48). That means display_init()
//       MUST run before touch_init(); a panel reset pulse after the
//       touch IC is up will yank the touch controller out from under
//       the LVGL pointer indev.
// ---------------------------------------------------------------------------
#pragma once

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/i2c_master.h"
#include "driver/i2s_common.h"

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

// === Shared I2C bus (touch + ES8311/ES7210 codec control) ====================
#define BOARD_I2C_PORT              I2C_NUM_0
#define BOARD_I2C_PIN_SDA           GPIO_NUM_8
#define BOARD_I2C_PIN_SCL           GPIO_NUM_18
#define BOARD_I2C_HZ                400000

// === Touch: TT21100 (BOX-3B variant) ========================================
#define BOARD_TOUCH_PIN_INT         GPIO_NUM_3
#define BOARD_TOUCH_PIN_RST         BOARD_LCD_PIN_RST  // level-shifted, see header

// === Audio: ES8311 (out) + ES7210 (in) — defined for M3, unused in M1 =======
#define BOARD_I2S_PORT              I2S_NUM_0
#define BOARD_I2S_PIN_MCLK          GPIO_NUM_2
#define BOARD_I2S_PIN_BCLK          GPIO_NUM_17
#define BOARD_I2S_PIN_LRCK          GPIO_NUM_45
#define BOARD_I2S_PIN_DOUT          GPIO_NUM_15   // ESP -> ES8311
#define BOARD_I2S_PIN_DIN           GPIO_NUM_16   // ESP <- ES7210
#define BOARD_AUDIO_PIN_PA_EN       GPIO_NUM_46   // speaker amp enable
#define BOARD_ES8311_I2C_ADDR       (0x18)
#define BOARD_ES7210_I2C_ADDR       (0x40)

// === User inputs — defined for M4, unused in M1 ==============================
#define BOARD_BTN_PIN_BOOT          GPIO_NUM_0    // momentary, active-low
#define BOARD_MUTE_PIN              GPIO_NUM_1    // slide switch state
