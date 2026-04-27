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
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,   // BOX-3 wires panel BGR
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // ILI9342 ships with colour inversion ON for typical mounting — same as
    // ST7789, the Arduino BSP, and the Waveshare predecessor.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    // Landscape: swap_xy + mirror x. The mirror combo we want was confirmed
    // empirically on the BOX-3; if your panel comes up flipped, try mirror(false, true)
    // or (true, true) — only one of the four combinations renders right-side-up.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // ---- Clear to navy so we can tell "panel up" from "panel dark" ----------
    size_t   pixels = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;
    uint16_t *buf   = heap_caps_malloc(pixels * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) {
        // ILI9342 expects RGB565 big-endian on the SPI wire; ESP32 is little-
        // endian, so byte-swap before fill. Without this, navy decodes as a
        // dim green — same gotcha as ST7789, see git log for the history.
        const uint16_t navy = __builtin_bswap16(0x0014);
        for (size_t i = 0; i < pixels; ++i) buf[i] = navy;
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, buf);
        free(buf);
    } else {
        ESP_LOGW(TAG, "no PSRAM for clear buffer; skipping");
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
