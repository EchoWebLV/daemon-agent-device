// ---------------------------------------------------------------------------
//  LVGL bring-up — ties esp_lvgl_port to the already-initialised ST7789
//  panel and drops a hello label on the root screen. See ui.h.
// ---------------------------------------------------------------------------
#include "ui.h"
#include "display.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "ui";

// Tap counter on the hello screen. Lets the touch bring-up have a visible,
// end-to-end "did LVGL see that finger?" signal. Keeping the state here
// instead of in touch.c keeps the boundary clean: touch.c talks to the
// CST328; ui.c owns widgets.
static uint32_t s_tap_count = 0;
static lv_obj_t *s_tap_label = NULL;

static void hello_btn_clicked(lv_event_t *e) {
    (void)e;
    s_tap_count++;
    if (s_tap_label) {
        lv_label_set_text_fmt(s_tap_label, "Taps: %" PRIu32, s_tap_count);
    }
}

// 40 scan-line partial buffer, double-buffered, in DMA-capable internal RAM.
// 2 * 240 * 40 * 2 B = 38.4 KB — comfortable next to the ~316 KB free heap
// we had at boot. PSRAM would work too but internal RAM is faster and the
// buffer is small enough to keep there.
#define LVGL_BUF_LINES 40

esp_err_t ui_init(void) {
    // --- LVGL task: runs lv_timer_handler every ~5 ms on core 1, prio 2. ----
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    // --- Display wiring -----------------------------------------------------
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = display_io(),
        .panel_handle  = display_panel(),
        .buffer_size   = DISPLAY_WIDTH * LVGL_BUF_LINES,
        .double_buffer = true,
        .hres          = DISPLAY_WIDTH,
        .vres          = DISPLAY_HEIGHT,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
            // ST7789 wants RGB565 big-endian on the wire; let the port layer
            // handle it instead of byte-swapping pixels by hand everywhere.
            .swap_bytes  = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "lvgl_port_add_disp");

    // --- Hello screen. LVGL isn't thread-safe; use the port mutex. ----------
    if (lvgl_port_lock(0)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x001428), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr,   LV_OPA_COVER,             LV_PART_MAIN);

        lv_obj_t *title = lv_label_create(scr);
        lv_label_set_text(title, "Daemon");
        lv_obj_set_style_text_color(title, lv_color_hex(0xE6E6FA), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

        lv_obj_t *sub = lv_label_create(scr);
        lv_label_set_text(sub, "LVGL v9 on ESP-IDF 5.5");
        lv_obj_set_style_text_color(sub, lv_color_hex(0x7A9BFF), LV_PART_MAIN);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, -38);

        // Big tappable button so touch regressions show up immediately.
        // Click handler bumps a counter; label below reflects it.
        lv_obj_t *btn = lv_button_create(scr);
        lv_obj_set_size(btn, 160, 60);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, 10);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E3A8A), LV_PART_MAIN);
        lv_obj_add_event_cb(btn, hello_btn_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t *btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, "Tap me");
        lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF),
                                    LV_PART_MAIN);
        lv_obj_center(btn_label);

        s_tap_label = lv_label_create(scr);
        lv_label_set_text(s_tap_label, "Taps: 0");
        lv_obj_set_style_text_color(s_tap_label, lv_color_hex(0xCBD5E1),
                                    LV_PART_MAIN);
        lv_obj_align(s_tap_label, LV_ALIGN_CENTER, 0, 70);

        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "LVGL up: %dx%d, %u KB partial buffers (internal RAM)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT,
             (unsigned)(2 * DISPLAY_WIDTH * LVGL_BUF_LINES * 2 / 1024));
    return ESP_OK;
}
