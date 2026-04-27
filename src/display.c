// ---------------------------------------------------------------------------
//  display.c — ILI9342C bring-up for the ESP32-S3-BOX-3B.
//
//  Uses esp_lcd over SPI3 (BOX-3 routes the LCD bus to SPI3, NOT SPI2 like
//  the Waveshare predecessor). Driver comes from espressif/esp_lcd_ili9341,
//  which covers both ILI9341 and ILI9342C parts (the IC inside BOX-3 is a
//  9342C — same command set as 9341, slightly different gamma defaults; the
//  driver handles both transparently).
//
//  Orientation: panel is mounted as 320x240 landscape. We achieve that by
//  swapping x/y and mirroring x — same trick as the Arduino BSP.
//
//  Public API (display_init / display_panel / display_io / DISPLAY_WIDTH /
//  DISPLAY_HEIGHT) is unchanged so ui.c and the *_screen.c files compile
//  without modification.
//
//  Init order constraint: this MUST run before touch_init(). Touch RST is
//  level-shifted from LCD_RST (GPIO48); a late panel-reset pulse will yank
//  the touch IC out from under the LVGL pointer indev. board.h documents
//  the same.
// ---------------------------------------------------------------------------
#include "display.h"
#include "board.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "display";

#define LCD_CMD_BITS   8
#define LCD_PARAM_BITS 8

// Vendor-specific init commands for the BOX-3's panel (ILI9342C). Lifted
// verbatim from espressif/esp-bsp's bsp/esp-box-3/esp-box-3.c. Without
// this, the ILI9341 driver's defaults give wrong gamma + the wrong base
// MADCTL — symptoms: face shows up purple, orientation rotated wrong.
static const ili9341_lcd_init_cmd_t box3_panel_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},
    {0xB1, (uint8_t []){0x00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},   // MADCTL: BGR bit only, no rotation/mirror at panel level
    {0x3A, (uint8_t []){0x55}, 1, 0},   // COLMOD: 16-bit / RGB565
    {0xB7, (uint8_t []){0x06}, 1, 0},
    {0x11, (uint8_t []){0}, 0x80, 0},   // SLPOUT
    {0x29, (uint8_t []){0}, 0x80, 0},   // DISPON
    {0,    (uint8_t []){0}, 0xff, 0},
};

static esp_lcd_panel_handle_t    s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io    = NULL;

esp_err_t display_init(void) {
    // ---- SPI bus (SPI3 on BOX-3) -------------------------------------------
    // max_transfer_sz sized so half a landscape frame fits one DMA burst:
    // 320 * 120 * 2 B (RGB565) = 76.8 KB. LVGL's two flush buffers are well
    // under this cap.
    spi_bus_config_t buscfg = {
        .sclk_io_num     = BOARD_LCD_PIN_SCLK,
        .mosi_io_num     = BOARD_LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 120 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // ---- Panel IO (SPI-mode wrapper around the bus) ------------------------
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = BOARD_LCD_PIN_DC,
        .cs_gpio_num       = BOARD_LCD_PIN_CS,
        .pclk_hz           = BOARD_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_io));

    // ---- ILI9342C panel ----------------------------------------------------
    // CRITICAL: BOX-3's RST line is ACTIVE HIGH (verified against esp-bsp
    // bsp/esp-box-3/esp-box-3.c). Without this flag the panel ends up STUCK
    // IN RESET after init (LOW pulse then HIGH = reset asserted), and the
    // touch IC — which shares this line through a level shifter — also
    // stays in reset. Symptoms: panel backlight on but pure white forever,
    // touch IC absent from the I2C bus. Hours of debugging right here.
    const ili9341_vendor_config_t vendor_config = {
        .init_cmds      = box3_panel_init,
        .init_cmds_size = sizeof(box3_panel_init) / sizeof(box3_panel_init[0]),
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num            = BOARD_LCD_PIN_RST,
        .flags.reset_active_high   = 1,
        .rgb_ele_order             = LCD_RGB_ELEMENT_ORDER_BGR,   // BOX-3 wires panel BGR
        .bits_per_pixel            = 16,
        .vendor_config             = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // Orientation: vendor_specific_init's MADCTL=0x08 already sets BGR-with-
    // no-rotation. The BSP layers mirror(true, true) on top, but that turns
    // out to leave the face upside-down on this device variant. Dropping to
    // no extra mirror lands the image right-side up.
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // ---- Clear to navy so we can tell "panel up" from "panel dark" ----------
    // Buffer lives in INTERNAL RAM, not PSRAM. SPI master can't DMA directly
    // from Octal PSRAM on the S3 — it tries to allocate a "priv TX buffer"
    // in internal RAM as an intermediate, which fails for a 150 KB full-
    // frame draw. So we paint the panel one strip at a time with a small
    // (~24 KB) DMA-capable buffer that fits inside max_transfer_sz.
    const size_t STRIP_LINES = 30;                                // 320*30*2 = 18.75 KB
    const size_t STRIP_BYTES = (size_t)DISPLAY_WIDTH * STRIP_LINES * sizeof(uint16_t);
    uint16_t *strip = heap_caps_malloc(STRIP_BYTES,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (strip) {
        // ILI9342 expects RGB565 big-endian on the SPI wire; ESP32 is little-
        // endian, so byte-swap before fill. Same gotcha as ST7789.
        const uint16_t navy = __builtin_bswap16(0x0014);
        size_t cells = (size_t)DISPLAY_WIDTH * STRIP_LINES;
        for (size_t i = 0; i < cells; ++i) strip[i] = navy;
        for (int y = 0; y < DISPLAY_HEIGHT; y += STRIP_LINES) {
            int h = (y + STRIP_LINES > DISPLAY_HEIGHT) ? (DISPLAY_HEIGHT - y) : STRIP_LINES;
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, DISPLAY_WIDTH, y + h, strip);
        }
        free(strip);
    } else {
        ESP_LOGW(TAG, "no internal-RAM DMA buffer for clear; skipping");
    }

    ESP_LOGI(TAG, "ILI9342C %dx%d up @ %d MHz on SPI3",
             DISPLAY_WIDTH, DISPLAY_HEIGHT,
             BOARD_LCD_PIXEL_CLOCK_HZ / 1000000);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_panel(void) {
    return s_panel;
}

esp_lcd_panel_io_handle_t display_io(void) {
    return s_io;
}
