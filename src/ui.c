// ---------------------------------------------------------------------------
//  LVGL bring-up + screen wiring.
//
//  Owns:
//    • esp_lvgl_port startup + display attach
//    • every screen (creature / menu / wallet / info / config / settings / wifi)
//    • the cross-screen broadcast of status/price/usdc labels
//
//  Navigation (see screen_anim_to for the semantic grid):
//    creature  ←left→ menu              creature  ←right→ settings  ↓ wifi
//    menu      taps:  Wallet / Info / Config (all swipe-right back to menu)
//  Swipes are installed on each screen's root in install_swipe_handlers().
// ---------------------------------------------------------------------------
#include "ui.h"
#include "config_screen.h"
#include "creature_screen.h"
#include "display.h"
#include "info_screen.h"
#include "menu_screen.h"
#include "settings_screen.h"
#include "voice.h"
#include "wallet_screen.h"
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
// Vertical axes only. Swipe UP reveals the menu from the bottom; swipe
// DOWN reveals settings from the top. The leaves (wallet / info / config)
// return to the menu with swipe DOWN; the menu returns to the creature
// the same way. Settings returns to creature with swipe UP. Wi-Fi keeps
// its DOWN-to-dismiss so the modal still has an escape.
//
// lv_indev_wait_release swallows the gesture so the finger-lift doesn't
// click whatever widget it lands on in the next screen.
static void on_gesture_creature(lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    switch (d) {
        case LV_DIR_TOP:    ui_show_menu();     break;  // swipe up
        case LV_DIR_BOTTOM: ui_show_settings(); break;  // swipe down
        default: break;
    }
}

// Menu + its three leaf screens (wallet / info / config) all swipe DOWN
// to go "back". The leaves return to the menu; the menu itself returns to
// the creature so the home-swipe is reachable in at most two gestures.
static void on_gesture_menu(lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    if (d == LV_DIR_BOTTOM) ui_show_creature();
}

static void on_gesture_back_to_menu(lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    if (d == LV_DIR_BOTTOM) ui_show_menu();
}

static void on_gesture_settings(lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    if (d == LV_DIR_TOP) ui_show_creature();  // swipe up → back
}

static void on_gesture_wifi(lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    if (d == LV_DIR_BOTTOM) ui_show_settings();
}

static void install_swipe_handlers(void) {
    if (lvgl_port_lock(0)) {
        lv_obj_add_event_cb(creature_screen(), on_gesture_creature,     LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(menu_screen(),     on_gesture_menu,         LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(wallet_screen(),   on_gesture_back_to_menu, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(info_screen(),     on_gesture_back_to_menu, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(config_screen(),   on_gesture_back_to_menu, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(settings_screen(), on_gesture_settings,     LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(wifi_screen(),     on_gesture_wifi,         LV_EVENT_GESTURE, NULL);
        lvgl_port_unlock();
    }
}

// Dial the gesture threshold down from LVGL's default 50 px. 25 px is a
// thumbnail-sized flick, still well above sub-pixel rest jitter from the
// touch IC. Velocity min stays at the default 3 px/sample so hand-jitter
// doesn't register as phantom swipes. Call AFTER touch_init() so the
// pointer indev exists.
void ui_tune_gestures(void) {
    if (!lvgl_port_lock(0)) return;
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_set_gesture_min_distance(indev, 25);
        }
    }
    lvgl_port_unlock();
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
        // MUST match esp_lcd_panel_mirror() in display.c (both true,true).
        // BSP comment: "Rotation values must be same as used in esp_lcd for
        // initial settings of the screen." Keeping the two layers in sync
        // is what gets the rendered content right-side up on the BOX-3.
        .rotation = {
            .swap_xy  = false,
            .mirror_x = true,
            .mirror_y = true,
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
    ESP_RETURN_ON_FALSE(menu_screen_init(),     ESP_FAIL, TAG, "menu_screen_init");
    ESP_RETURN_ON_FALSE(wallet_screen_init(),   ESP_FAIL, TAG, "wallet_screen_init");
    ESP_RETURN_ON_FALSE(info_screen_init(),     ESP_FAIL, TAG, "info_screen_init");
    ESP_RETURN_ON_FALSE(config_screen_init(),   ESP_FAIL, TAG, "config_screen_init");
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
//
// Each transition animates at ~220 ms so swipes read as "slides" without
// fighting the user's next touch. Direction is derived from which screens
// we're transitioning between — see screen_anim_to() for the layout.

// Track the last screen we loaded so screen_anim_to() can pick a direction
// that matches where the user "came from". Defaults to "creature" because
// ui_init() drops the creature on top as the very first load.
static const char *s_last_screen = "creature";

// Semantic layout — the stack is now vertical, matching the swipe axis:
//
//     (swipe up)       menu / wallet / info / config
//                                    ↑
//                                 creature
//                                    ↓
//     (swipe down)              settings  →  wifi
//
// Two animation families: "swipe up" slides everything up and brings the
// new screen in from below (MOVE_TOP). "swipe down" slides everything down
// and brings the new screen in from above (MOVE_BOTTOM). Menu→leaf is a
// tap (no swipe direction to honour) so it fades in place.
static lv_screen_load_anim_t screen_anim_to(const char *from, const char *to) {
    // Explicit per-pair table. Easier to reason about than position math
    // once the stack has multiple leaves.
    static const struct {
        const char *from, *to;
        lv_screen_load_anim_t anim;
    } TABLE[] = {
        // creature <-> menu (menu reveals from bottom, dismisses upward)
        { "creature", "menu",     LV_SCR_LOAD_ANIM_MOVE_TOP    },
        { "menu",     "creature", LV_SCR_LOAD_ANIM_MOVE_BOTTOM },
        // creature <-> settings (settings reveals from top, dismisses down)
        { "creature", "settings", LV_SCR_LOAD_ANIM_MOVE_BOTTOM },
        { "settings", "creature", LV_SCR_LOAD_ANIM_MOVE_TOP    },
        // settings <-> wifi (wifi pops up from the bottom, drops back down)
        { "settings", "wifi",     LV_SCR_LOAD_ANIM_MOVE_TOP    },
        { "wifi",     "settings", LV_SCR_LOAD_ANIM_MOVE_BOTTOM },
        // leaf → menu via swipe-down. Menu → leaf uses the fade fallback
        // so the tap doesn't fight a non-existent swipe direction.
        { "wallet",   "menu",     LV_SCR_LOAD_ANIM_MOVE_BOTTOM },
        { "info",     "menu",     LV_SCR_LOAD_ANIM_MOVE_BOTTOM },
        { "config",   "menu",     LV_SCR_LOAD_ANIM_MOVE_BOTTOM },
    };
    for (size_t i = 0; i < sizeof(TABLE)/sizeof(TABLE[0]); i++) {
        if (!strcmp(from, TABLE[i].from) && !strcmp(to, TABLE[i].to)) {
            return TABLE[i].anim;
        }
    }
    return LV_SCR_LOAD_ANIM_FADE_IN;
}

// Centralised loader so every ui_show_* shares the same timing + bookkeeping.
// auto_del=false because each screen is built once in ui_init() and reused;
// we don't want LVGL tearing them down between transitions.
static void load_screen_anim(lv_obj_t *target, const char *name) {
    lv_screen_load_anim_t anim = screen_anim_to(s_last_screen, name);
    if (lvgl_port_lock(0)) {
        lv_screen_load_anim(target, anim, 220, 0, false);
        lvgl_port_unlock();
    }
    // Safe: every caller passes a literal. Keep in sync even if the lock
    // failed — LVGL won't have switched, but the next load_screen_anim call
    // will just compute an animation that happens to land on the same target.
    s_last_screen = name;
}

void ui_show_creature(void) {
    load_screen_anim(creature_screen(), "creature");
}

void ui_show_menu(void) {
    load_screen_anim(menu_screen(), "menu");
}

void ui_show_wallet(void) {
    load_screen_anim(wallet_screen(), "wallet");
    wallet_screen_refresh();
}

void ui_show_info(void) {
    load_screen_anim(info_screen(), "info");
    info_screen_refresh();
}

void ui_show_config(void) {
    load_screen_anim(config_screen(), "config");
    config_screen_refresh();
}

void ui_show_settings(void) {
    load_screen_anim(settings_screen(), "settings");
    settings_screen_refresh();
}

void ui_show_wifi(void) {
    load_screen_anim(wifi_screen(), "wifi");
}

// --- broadcasts ------------------------------------------------------------

void ui_set_status(const char *s) {
    creature_screen_set_status(s);
    menu_screen_set_status(s);
    wallet_screen_set_status(s);
    info_screen_set_status(s);
    config_screen_set_status(s);
    settings_screen_set_status(s);
    wifi_screen_set_status(s);
}

void ui_set_price(const char *s) {
    creature_screen_set_price(s);
    menu_screen_set_price(s);
    wallet_screen_set_price(s);
    info_screen_set_price(s);
    config_screen_set_price(s);
    settings_screen_set_price(s);
    wifi_screen_set_price(s);
}

void ui_set_usdc(const char *s) {
    creature_screen_set_usdc(s);
    menu_screen_set_usdc(s);
    wallet_screen_set_usdc(s);
    info_screen_set_usdc(s);
    config_screen_set_usdc(s);
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
            // Wipe the subtitle so the face returns to a clean idle state
            // once the audio has drained. Keeping the reply on-screen past
            // the spoken line makes the face feel "stuck".
            creature_screen_set_subtitle("");
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

void ui_refresh_info(void) {
    info_screen_refresh();
}

void ui_refresh_config(void) {
    config_screen_refresh();
}

// --- Test-harness bridges --------------------------------------------------

const char *ui_current_screen_name(void) {
    // Compare the active screen pointer to each of the cached screen roots.
    // Reads lv_screen_active() under the port lock so we never race the LVGL
    // task mid-transition. Returns a literal — the comparison is only needed
    // while we hold the lock.
    const char *name = "unknown";
    if (lvgl_port_lock(0)) {
        lv_obj_t *a = lv_screen_active();
        if      (a == creature_screen()) name = "creature";
        else if (a == menu_screen())     name = "menu";
        else if (a == wallet_screen())   name = "wallet";
        else if (a == info_screen())     name = "info";
        else if (a == config_screen())   name = "config";
        else if (a == settings_screen()) name = "settings";
        else if (a == wifi_screen())     name = "wifi";
        lvgl_port_unlock();
    }
    return name;
}

void ui_inject_swipe(int direction) {
    // Mirrors the on_gesture_* handlers above. Direction encoding matches
    // the TEST SWIPE verb: 0=LEFT, 1=RIGHT, 2=UP, 3=DOWN.
    const char *from = ui_current_screen_name();

    if (!strcmp(from, "creature")) {
        if      (direction == 2) ui_show_menu();      // UP
        else if (direction == 3) ui_show_settings();  // DOWN
    } else if (!strcmp(from, "menu")) {
        if (direction == 3) ui_show_creature();       // DOWN → back
    } else if (!strcmp(from, "wallet") ||
               !strcmp(from, "info")   ||
               !strcmp(from, "config")) {
        if (direction == 3) ui_show_menu();           // DOWN → back to menu
    } else if (!strcmp(from, "settings")) {
        if (direction == 2) ui_show_creature();       // UP → back
    } else if (!strcmp(from, "wifi")) {
        if (direction == 3) ui_show_settings();       // DOWN → dismiss
    }
}

void ui_force_repaint(void) {
    // lv_refr_now drives a full dirty-area flush synchronously on the caller's
    // thread while we hold the port lock. The harness wraps this in
    // esp_timer_get_time() calls to time the paint.
    if (lvgl_port_lock(0)) {
        lv_obj_t *a = lv_screen_active();
        if (a) lv_obj_invalidate(a);
        lv_refr_now(NULL);
        lvgl_port_unlock();
    }
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
