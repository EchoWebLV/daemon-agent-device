// ---------------------------------------------------------------------------
//  ST7789 display bring-up. Pins match the Arduino TFT_eSPI setup so the
//  same physical board keeps working without hardware changes.
// ---------------------------------------------------------------------------
#include "display.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "display";

// ---- Pin map (Waveshare 2.8") -----------------------------------------------
#define PIN_MOSI   45
#define PIN_SCLK   40
#define PIN_CS     42
#define PIN_DC     41
#define PIN_RST    39
#define PIN_BL     5

// SPI2 on the S3 is the general-purpose GP-SPI host; SPI3 has more DMA
// restrictions on certain pin combos so we stick with SPI2 here.
#define LCD_HOST   SPI2_HOST

// 80 MHz is the sweet spot for this ST7789 panel — we confirmed it on the
// Arduino build and pushed full-sprite refresh in ~15 ms.
#define PIXEL_CLOCK_HZ (80 * 1000 * 1000)

// ST7789 LCD command + parameter widths. Both 8-bit for this part.
#define LCD_CMD_BITS   8
#define LCD_PARAM_BITS 8

static esp_lcd_panel_handle_t s_panel = NULL;

esp_err_t display_init(void) {
    // ---- Backlight GPIO (off during init to hide the bring-up flash) --------
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << PIN_BL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(PIN_BL, 0);

    // ---- SPI bus ------------------------------------------------------------
    // max_transfer_sz picks the upper bound on a single DMA-enabled SPI burst.
    // Sizing for 80 scan lines @ 240 px * 2 B (RGB565) lets us push half the
    // screen in one go, which LVGL is happy with.
    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_SCLK,
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // ---- Panel IO (SPI-mode wrapper around the bus) -------------------------
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_DC,
        .cs_gpio_num       = PIN_CS,
        .pclk_hz           = PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    // ---- ST7789 panel -------------------------------------------------------
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // ST7789 ships with colour inversion required to show correct colours.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // ---- Clear to a solid colour so we know the pipeline works --------------
    // Use PSRAM since the full-frame buffer is 240*320*2 = 150 KB, which is
    // too big to waste on internal RAM for a one-off clear.
    size_t   pixels = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;
    uint16_t *buf   = heap_caps_malloc(pixels * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) {
        // Muted navy — deliberately not pure black so "display is dark" vs
        // "display is working but drew black" is distinguishable.
        //
        // ST7789 expects RGB565 big-endian on the SPI wire; the ESP32 is
        // little-endian, so we byte-swap before the fill. Without this, the
        // panel reads {LSB, MSB} and 0x0014 (navy) decodes as 0x1400 — a
        // medium green with a faint red tint. Ask me how I know.
        const uint16_t navy = __builtin_bswap16(0x0014);  // RGB565 dark blue, wire order
        for (size_t i = 0; i < pixels; ++i) buf[i] = navy;
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, buf);
        free(buf);
    } else {
        ESP_LOGW(TAG, "no PSRAM for clear buffer; skipping");
    }

    // ---- Backlight on -------------------------------------------------------
    gpio_set_level(PIN_BL, 1);
    ESP_LOGI(TAG, "ST7789 %dx%d up @ %d MHz SPI",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, PIXEL_CLOCK_HZ / 1000000);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_panel(void) {
    return s_panel;
}
