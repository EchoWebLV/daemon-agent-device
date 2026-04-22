// ---------------------------------------------------------------------------
//  LVGL bring-up + screen wiring.
//
//  Owns:
//    • esp_lvgl_port startup + display attach
//    • the four Daemon screens (creature / wallet / settings / wifi)
//    • the cross-screen broadcast of status/price/usdc labels
//
//  Navigation today is limited: boot shows creature, settings' Wi-Fi row
//  navigates to wifi_screen, and a successful connect returns to settings.
//  Swipe-based screen cycling comes with the phase-7 integration pass.
// ---------------------------------------------------------------------------
#include "ui.h"
#include "display.h"
#include "creature_screen.h"
#include "wallet_screen.h"
#include "settings_screen.h"
#include "wifi_screen.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "ui";

// 40 scan-line partial buffer, double-buffered, in DMA-capable internal RAM.
// 2 * 240 * 40 * 2 B = 38.4 KB. Comfortable next to the ~316 KB internal
// heap free at boot; keeps LVGL's draw path DMA-fed without touching PSRAM.
#define LVGL_BUF_LINES 40

// --- navigation plumbed through callbacks ----------------------------------

static void nav_go_to_wifi(void) {
    ui_show_wifi();
    wifi_screen_kick_scan();
}

static void nav_wifi_connected(void) {
    // Wi-Fi picker bounces back to settings so the user sees the newly-
    // connected SSID in the Wi-Fi row.
    ui_show_settings();
    settings_screen_refresh();
}

// --- public API ------------------------------------------------------------

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

    // --- Screens. Each module locks the port mutex itself; we just call
    //     the init entry points in order and load the creature on top. -----
    ESP_RETURN_ON_FALSE(creature_screen_init(), ESP_FAIL, TAG, "creature_screen_init");
    ESP_RETURN_ON_FALSE(wallet_screen_init(),   ESP_FAIL, TAG, "wallet_screen_init");
    ESP_RETURN_ON_FALSE(settings_screen_init(), ESP_FAIL, TAG, "settings_screen_init");
    ESP_RETURN_ON_FALSE(wifi_screen_init(),     ESP_FAIL, TAG, "wifi_screen_init");

    // Wire the two explicit transitions the screens themselves trigger.
    settings_screen_on_wifi_click(nav_go_to_wifi);
    wifi_screen_on_connected(nav_wifi_connected);

    // Land on the creature.
    if (lvgl_port_lock(0)) {
        lv_screen_load(creature_screen());
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "LVGL up: %dx%d, %u KB partial buffers (internal RAM)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT,
             (unsigned)(2 * DISPLAY_WIDTH * LVGL_BUF_LINES * 2 / 1024));
    return ESP_OK;
}

// --- screen switchers ------------------------------------------------------

void ui_show_creature(void) {
    if (lvgl_port_lock(0)) {
        lv_screen_load(creature_screen());
        lvgl_port_unlock();
    }
}

void ui_show_wallet(void) {
    if (lvgl_port_lock(0)) {
        lv_screen_load(wallet_screen());
        lvgl_port_unlock();
    }
    wallet_screen_refresh();
}

void ui_show_settings(void) {
    if (lvgl_port_lock(0)) {
        lv_screen_load(settings_screen());
        lvgl_port_unlock();
    }
    settings_screen_refresh();
}

void ui_show_wifi(void) {
    if (lvgl_port_lock(0)) {
        lv_screen_load(wifi_screen());
        lvgl_port_unlock();
    }
}

// --- broadcasts ------------------------------------------------------------

void ui_set_status(const char *s) {
    creature_screen_set_status(s);
    wallet_screen_set_status(s);
    settings_screen_set_status(s);
    wifi_screen_set_status(s);
}

void ui_set_price(const char *s) {
    creature_screen_set_price(s);
    wallet_screen_set_price(s);
    settings_screen_set_price(s);
    wifi_screen_set_price(s);
}

void ui_set_usdc(const char *s) {
    creature_screen_set_usdc(s);
    wallet_screen_set_usdc(s);
    settings_screen_set_usdc(s);
    wifi_screen_set_usdc(s);
}

void ui_set_subtitle(const char *s) {
    creature_screen_set_subtitle(s);
}

void ui_set_mood(ui_mood_t m) {
    creature_screen_set_mood((creature_mood_t)m);
}

void ui_set_talking(bool on) {
    creature_screen_set_talking(on);
}

void ui_tick(void) {
    creature_screen_tick();
}

void ui_refresh_wallet(void) {
    wallet_screen_refresh();
}

void ui_refresh_settings(void) {
    settings_screen_refresh();
}
