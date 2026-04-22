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
#include "voice.h"
#include "wifi_screen.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "ui";

// 20 scan-line partial buffer, double-buffered, in DMA-capable internal RAM.
// 2 * 240 * 20 * 2 B = 19.2 KB. The board has ~228 KB internal free at boot,
// but by the time LVGL allocates buf2, fragmentation can break up the larger
// contiguous runs — 40-line buffers crashed with "Not enough memory for buf2"
// on real silicon, so we keep each buffer under 10 KB to stay well inside
// whatever contiguous block the allocator has left. Trade-off: LVGL flushes
// partials more often, but the SPI link at 80 MHz eats the extra calls.
#define LVGL_BUF_LINES 20

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

// --- swipe navigation -------------------------------------------------------
//
// Each screen's root gets a LV_EVENT_GESTURE listener wired to a screen-
// specific dispatch table. We swallow the gesture with lv_indev_wait_release
// so the swipe doesn't get interpreted as a subsequent click on whatever
// widget happens to be under the finger when it lifts.

static void on_gesture_creature(lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    switch (d) {
        case LV_DIR_LEFT:  ui_show_wallet();   break;
        case LV_DIR_RIGHT: ui_show_settings(); break;
        default: break;
    }
}

static void on_gesture_wallet(lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    switch (d) {
        case LV_DIR_RIGHT: ui_show_creature(); break;
        default: break;
    }
}

static void on_gesture_settings(lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    switch (d) {
        case LV_DIR_LEFT: ui_show_creature(); break;
        default: break;
    }
}

static void on_gesture_wifi(lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    switch (d) {
        // DOWN cancels the Wi-Fi picker and returns to settings, matching
        // the "pull down to dismiss" modal idiom most users already know.
        case LV_DIR_BOTTOM: ui_show_settings(); break;
        default: break;
    }
}

static void install_swipe_handlers(void) {
    if (lvgl_port_lock(0)) {
        lv_obj_add_event_cb(creature_screen(), on_gesture_creature, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(wallet_screen(),   on_gesture_wallet,   LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(settings_screen(), on_gesture_settings, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(wifi_screen(),     on_gesture_wifi,     LV_EVENT_GESTURE, NULL);
        lvgl_port_unlock();
    }
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

    // Swipe listeners on each screen's root. Must run after the screens are
    // built so the roots exist.
    install_swipe_handlers();

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

    // Voice-driven mouth sync. The audio task owns voice_is_speaking(); we
    // just flip the creature's talking flag on the transitions so the mouth
    // closes itself when the buffer runs dry and the mood returns to idle.
    // Edge-triggered — no work in steady state.
    static bool last_speaking = false;
    bool speaking = voice_is_speaking();
    if (speaking != last_speaking) {
        creature_screen_set_talking(speaking);
        if (!speaking) {
            creature_screen_set_mood(CREATURE_MOOD_IDLE);
        }
        last_speaking = speaking;
    }
}

void ui_refresh_wallet(void) {
    wallet_screen_refresh();
}

void ui_refresh_settings(void) {
    settings_screen_refresh();
}

// --- AI reply bridge -------------------------------------------------------

void ui_deliver_reply(const char *text) {
    if (!text || !text[0]) return;

    // Subtitle under the creature, mood → TALK, mouth on, and snap to the
    // creature screen so the user sees the face regardless of where they
    // were when the reply landed. ui_tick() will flip talking off once the
    // voice task drains the audio stream.
    creature_screen_set_subtitle(text);
    creature_screen_set_mood(CREATURE_MOOD_TALK);
    creature_screen_set_talking(true);
    ui_show_creature();

    // Kick the voice task. Returns immediately; audio streams out on the
    // voice task without blocking the /say HTTP response path.
    voice_speak(text);
}
