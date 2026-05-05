// ---------------------------------------------------------------------------
//  Settings screen implementation.
//
//  Event handlers all run under lvgl_port's mutex (LVGL dispatches from
//  that context), so they can touch widgets directly but must be careful
//  if they call into non-LVGL subsystems (devcfg_set_* is fine; it writes
//  NVS but that's independent of the port mutex).
// ---------------------------------------------------------------------------
#include "settings_screen.h"
#include "screens_common.h"
#include "devcfg.h"
#include "payapi.h"
#include "ui.h"
#include "voice.h"
#include "wifi_sta.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"

static const char *TAG = "settings_screen";

static lv_obj_t *s_scr = NULL;

// Status bar labels (top). price + usdc are retired on side screens; the
// setters below are no-ops so ui.c can broadcast without caring.
static lv_obj_t *s_status_label = NULL;

// Wi-Fi row.
static lv_obj_t *s_wifi_row   = NULL;
static lv_obj_t *s_wifi_title = NULL;
static lv_obj_t *s_wifi_sub   = NULL;

// Bluetooth row.
static lv_obj_t *s_bt_switch  = NULL;

// Volume slider.
static lv_obj_t *s_vol_slider = NULL;
static lv_obj_t *s_vol_value  = NULL;

// Brightness slider.
static lv_obj_t *s_bri_slider = NULL;
static lv_obj_t *s_bri_value  = NULL;

// Theme switch — toggles dark↔light. Requires a restart for live widgets
// to repaint, so the handler persists the new value and schedules an
// esp_restart() ~250 ms later (long enough for the user to see the switch
// land in the new position before the screen blacks out).
static lv_obj_t *s_theme_switch = NULL;

// Spending row value label — updated by settings_screen_refresh() so it
// reflects web-admin changes without a full screen rebuild.
static lv_obj_t *s_spend_row_value = NULL;

static settings_wifi_click_cb_t s_wifi_click_cb = NULL;

// --- shared row scaffold ---------------------------------------------------
//
// Returns a 1-row panel at (12, y_top), full width minus 24, 56 tall. The
// caller fills the inside with a title label + whatever control.
static lv_obj_t *make_row(lv_obj_t *parent, int y_top) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, SCR_W - 24, 56);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 12, y_top);
    lv_obj_set_style_bg_color(row, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

// --- event handlers --------------------------------------------------------

// Shared X close button (see wallet_screen.c for the canonical copy).
// Duplicated per screen rather than shoved into screens_common.h because
// this file already owns a row of its own event handlers, and adding a
// third include layer for ~20 lines of chrome wasn't worth the traversal.
static void close_clicked(lv_event_t *e) {
    (void)e;
    ui_show_creature();
}

static void add_close_button(lv_obj_t *bar) {
    lv_obj_t *btn = lv_button_create(bar);
    lv_obj_set_size(btn, 28, STATUS_BAR_H - 4);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, close_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *x = lv_label_create(btn);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_center(x);
}

// --- Spending detail screen -----------------------------------------------
//
// Full-screen overlay (on top of the settings screen) showing all three
// caps plus today's running total and a hint to use the web admin.
// Mirrors the approach used by wallet_screen.c sub-screens: create an
// lv_obj on lv_scr_act() and delete it from the Back button handler.

static void spend_detail_back_clicked(lv_event_t *e) {
    lv_obj_t *detail = (lv_obj_t *)lv_event_get_user_data(e);
    if (detail) lv_obj_delete(detail);
}

static void spend_row_clicked(lv_event_t *e) {
    (void)e;

    // Snapshot all values before taking the LVGL mutex.
    uint32_t auto_c    = devcfg_spend_auto_max_cents();
    uint32_t conf_c    = devcfg_spend_confirm_max_cents();
    uint32_t daily_c   = devcfg_spend_daily_cap_cents();
    uint64_t today_u   = devcfg_spend_today_micros();

    char ip[16] = "";
    if (wifi_sta_is_connected()) {
        wifi_sta_ip_str(ip, sizeof(ip));
    }

    // Build the hint string outside the mutex.
    char hint[64];
    if (ip[0]) {
        snprintf(hint, sizeof(hint), "Edit on web at %s", ip);
    } else {
        snprintf(hint, sizeof(hint), "Edit on web at this device's IP");
    }

    // today_u is in micros (1e-6 USD). Format as $X.XXXX.
    char today_str[32];
    snprintf(today_str, sizeof(today_str),
             "$%u.%04u",
             (unsigned)(today_u / 1000000ULL),
             (unsigned)((today_u % 1000000ULL) / 100ULL));

    // Full-screen overlay sitting on top of whatever is active.
    lv_obj_t *detail = lv_obj_create(lv_scr_act());
    lv_obj_set_size(detail, SCR_W, SCR_H);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(detail, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(detail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(detail, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(detail, 12, LV_PART_MAIN);
    lv_obj_remove_flag(detail, LV_OBJ_FLAG_SCROLLABLE);

    // Title.
    lv_obj_t *title = lv_label_create(detail);
    lv_label_set_text(title, "Spending");
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // Helper macro: add a label pair (key + value) at a given y offset.
    // We use raw lv_obj_create / lv_label_create calls to keep the style
    // consistent with the rest of the file.
    int dy = 28;

    // Auto cap.
    lv_obj_t *lbl_auto = lv_label_create(detail);
    lv_label_set_text_fmt(lbl_auto, "Auto-pay up to:   $%u.%02u",
                          (unsigned)(auto_c / 100u), (unsigned)(auto_c % 100u));
    lv_obj_set_style_text_color(lbl_auto, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(lbl_auto, LV_ALIGN_TOP_LEFT, 0, dy);
    dy += 22;

    // Confirm cap.
    lv_obj_t *lbl_conf = lv_label_create(detail);
    lv_label_set_text_fmt(lbl_conf, "Confirm up to:    $%u.%02u",
                          (unsigned)(conf_c / 100u), (unsigned)(conf_c % 100u));
    lv_obj_set_style_text_color(lbl_conf, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(lbl_conf, LV_ALIGN_TOP_LEFT, 0, dy);
    dy += 22;

    // Daily cap.
    lv_obj_t *lbl_daily = lv_label_create(detail);
    lv_label_set_text_fmt(lbl_daily, "Daily cap:        $%u.%02u",
                          (unsigned)(daily_c / 100u), (unsigned)(daily_c % 100u));
    lv_obj_set_style_text_color(lbl_daily, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(lbl_daily, LV_ALIGN_TOP_LEFT, 0, dy);
    dy += 22;

    // Divider gap.
    dy += 8;

    // Today's spend.
    lv_obj_t *lbl_today = lv_label_create(detail);
    lv_label_set_text_fmt(lbl_today, "Today's spend:    %s", today_str);
    lv_obj_set_style_text_color(lbl_today, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(lbl_today, LV_ALIGN_TOP_LEFT, 0, dy);
    dy += 28;

    // Hint.
    lv_obj_t *lbl_hint = lv_label_create(detail);
    lv_label_set_text(lbl_hint, hint);
    lv_obj_set_style_text_color(lbl_hint, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_label_set_long_mode(lbl_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_hint, SCR_W - 24);
    lv_obj_align(lbl_hint, LV_ALIGN_TOP_LEFT, 0, dy);

    // Back button — anchored to the bottom centre.
    lv_obj_t *back_btn = lv_button_create(detail);
    lv_obj_set_size(back_btn, 100, 36);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(back_btn, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, spend_detail_back_clicked, LV_EVENT_CLICKED, detail);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "Back");
    lv_obj_set_style_text_color(back_lbl, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_center(back_lbl);
}

static void wifi_row_clicked(lv_event_t *e) {
    (void)e;
    if (s_wifi_click_cb) s_wifi_click_cb();
}

static void bt_switch_changed(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    devcfg_set_bluetooth(on);
}

static void vol_slider_changed(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(sl);
    if (v < 0) v = 0;
    if (v > 21) v = 21;
    devcfg_set_volume((uint8_t)v);
    voice_set_volume((uint8_t)v);   // applies to the next sample written
    if (s_vol_value) lv_label_set_text_fmt(s_vol_value, "%" PRId32, v);
}

// Theme toggle handler. ui_apply_theme persists the new theme and rebuilds
// every cached screen with the new palette — no reboot, no Wi-Fi flap, no
// price-fetch reset. It defers the rebuild via lv_async_call, so this
// event handler returns cleanly before the firing widget gets deleted.
static void theme_switch_changed(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_apply_theme(checked ? UI_THEME_LIGHT : UI_THEME_DARK);
}

static void bri_slider_changed(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(sl);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    // devcfg_set_brightness() applies to the LEDC channel immediately.
    devcfg_set_brightness((uint8_t)v);
    if (s_bri_value) lv_label_set_text_fmt(s_bri_value, "%" PRId32, v);
}

// --- public API ------------------------------------------------------------

bool settings_screen_init(void) {
    if (s_scr) return true;
    if (!lvgl_port_lock(0)) { ESP_LOGE(TAG, "lvgl_port_lock failed"); return false; }

    s_scr = lv_obj_create(NULL);
    scr_apply_bg(s_scr);
    // Vertical scroll: scr_apply_bg removes the scrollable flag; re-add it
    // restricted to the vertical axis so the rows can grow past 240 px and
    // the user can flick down to reach later items.
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scr, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_AUTO);

    // --- status bar: title on the left, X on the right ---
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, SCR_W, STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 4, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(bar);
    lv_label_set_text(s_status_label, "settings");
    lv_obj_set_style_text_color(s_status_label, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    add_close_button(bar);

    // --- rows ---
    int y = STATUS_BAR_H + 10;

    // WIFI row (tappable)
    s_wifi_row = make_row(s_scr, y);
    lv_obj_add_flag(s_wifi_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wifi_row, wifi_row_clicked, LV_EVENT_CLICKED, NULL);

    s_wifi_title = lv_label_create(s_wifi_row);
    lv_label_set_text(s_wifi_title, "Wi-Fi");
    lv_obj_set_style_text_color(s_wifi_title, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_wifi_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_wifi_sub = lv_label_create(s_wifi_row);
    lv_label_set_text(s_wifi_sub, "not connected");
    lv_label_set_long_mode(s_wifi_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_wifi_sub, SCR_W - 56);
    lv_obj_set_style_text_color(s_wifi_sub, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_wifi_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *wifi_chevron = lv_label_create(s_wifi_row);
    lv_label_set_text(wifi_chevron, ">");
    lv_obj_set_style_text_color(wifi_chevron, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(wifi_chevron, LV_ALIGN_RIGHT_MID, 0, 0);

    y += 64;

    // BLUETOOTH row
    lv_obj_t *bt_row = make_row(s_scr, y);
    lv_obj_t *bt_title = lv_label_create(bt_row);
    lv_label_set_text(bt_title, "Bluetooth");
    lv_obj_set_style_text_color(bt_title, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(bt_title, LV_ALIGN_LEFT_MID, 0, 0);

    s_bt_switch = lv_switch_create(bt_row);
    lv_obj_align(s_bt_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_bt_switch, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bt_switch, SCR_COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_bt_switch, bt_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);

    y += 64;

    // VOLUME row
    lv_obj_t *vol_row = make_row(s_scr, y);
    lv_obj_t *vol_title = lv_label_create(vol_row);
    lv_label_set_text(vol_title, "Volume");
    lv_obj_set_style_text_color(vol_title, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(vol_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_vol_value = lv_label_create(vol_row);
    lv_label_set_text(s_vol_value, "0");
    lv_obj_set_style_text_color(s_vol_value, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(s_vol_value, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_vol_slider = lv_slider_create(vol_row);
    lv_slider_set_range(s_vol_slider, 0, 21);
    lv_obj_set_width(s_vol_slider, SCR_W - 56);
    lv_obj_align(s_vol_slider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_vol_slider, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, SCR_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, SCR_COLOR_ACCENT_HI, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_slider, vol_slider_changed, LV_EVENT_VALUE_CHANGED, NULL);

    y += 64;

    // BRIGHTNESS row
    lv_obj_t *bri_row = make_row(s_scr, y);
    lv_obj_t *bri_title = lv_label_create(bri_row);
    lv_label_set_text(bri_title, "Brightness");
    lv_obj_set_style_text_color(bri_title, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(bri_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_bri_value = lv_label_create(bri_row);
    lv_label_set_text(s_bri_value, "0");
    lv_obj_set_style_text_color(s_bri_value, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(s_bri_value, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_bri_slider = lv_slider_create(bri_row);
    lv_slider_set_range(s_bri_slider, 0, 255);
    lv_obj_set_width(s_bri_slider, SCR_W - 56);
    lv_obj_align(s_bri_slider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_bri_slider, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bri_slider, SCR_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bri_slider, SCR_COLOR_ACCENT_HI, LV_PART_KNOB);
    lv_obj_add_event_cb(s_bri_slider, bri_slider_changed, LV_EVENT_VALUE_CHANGED, NULL);

    y += 64;

    // SPENDING row (read-only; tap opens detail screen)
    lv_obj_t *spend_row = make_row(s_scr, y);
    lv_obj_add_flag(spend_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(spend_row, spend_row_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *spend_title = lv_label_create(spend_row);
    lv_label_set_text(spend_title, "Spending");
    lv_obj_set_style_text_color(spend_title, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(spend_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_spend_row_value = lv_label_create(spend_row);
    lv_label_set_text(s_spend_row_value, "$0.10 auto / $10.00 cap");
    lv_label_set_long_mode(s_spend_row_value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_spend_row_value, SCR_W - 56);
    lv_obj_set_style_text_color(s_spend_row_value, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_spend_row_value, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *spend_chevron = lv_label_create(spend_row);
    lv_label_set_text(spend_chevron, ">");
    lv_obj_set_style_text_color(spend_chevron, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(spend_chevron, LV_ALIGN_RIGHT_MID, 0, 0);

    y += 64;

    // THEME row — flip dark ↔ light. Toggling fires a delayed esp_restart
    // so every screen rebuilds against the freshly-applied palette.
    lv_obj_t *theme_row = make_row(s_scr, y);
    lv_obj_t *theme_title = lv_label_create(theme_row);
    lv_label_set_text(theme_title, "Light theme");
    lv_obj_set_style_text_color(theme_title, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(theme_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *theme_sub = lv_label_create(theme_row);
    lv_label_set_text(theme_sub, "device restarts when toggled");
    lv_obj_set_style_text_color(theme_sub, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(theme_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_theme_switch = lv_switch_create(theme_row);
    lv_obj_align(s_theme_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_theme_switch, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_theme_switch, SCR_COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (devcfg_theme() == DEVCFG_THEME_LIGHT) {
        lv_obj_add_state(s_theme_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_theme_switch, theme_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lvgl_port_unlock();

    // Seed current values.
    settings_screen_refresh();

    ESP_LOGI(TAG, "settings screen built");
    return true;
}

lv_obj_t *settings_screen(void) { return s_scr; }

void settings_screen_destroy(void) {
    if (!s_scr) return;
    if (!lvgl_port_lock(0)) return;
    lv_obj_delete(s_scr);
    s_status_label = NULL;
    s_wifi_row     = NULL;
    s_wifi_title   = NULL;
    s_wifi_sub     = NULL;
    s_bt_switch      = NULL;
    s_vol_slider     = NULL;
    s_vol_value      = NULL;
    s_bri_slider     = NULL;
    s_bri_value      = NULL;
    s_theme_switch   = NULL;
    s_spend_row_value = NULL;
    s_scr            = NULL;
    lvgl_port_unlock();
}

void settings_screen_set_status(const char *s) {
    if (!s_status_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_label, s ? s : "");
    lvgl_port_unlock();
}

void settings_screen_set_price(const char *s) {
    // No-op: ticker is creature-only now. See wallet_screen_set_price().
    (void)s;
}

void settings_screen_set_usdc(const char *s) {
    // Ditto.
    (void)s;
}

void settings_screen_refresh(void) {
    if (!s_scr) return;

    // Snapshot state into locals before taking the mutex so we don't hold
    // it across NVS / Wi-Fi driver calls (they're quick, but still).
    bool bt_on = devcfg_bluetooth();
    uint8_t vol = devcfg_volume();
    uint8_t bri = devcfg_brightness();

    char wifi_sub[96];
    if (wifi_sta_is_connected()) {
        char ip[16];
        wifi_sta_ip_str(ip, sizeof(ip));
        const char *ssid = wifi_sta_current_ssid();
        int8_t rssi = wifi_sta_current_rssi();
        snprintf(wifi_sub, sizeof(wifi_sub), "%s  |  %s  |  %d dBm",
                 ssid[0] ? ssid : "(unknown)",
                 ip[0] ? ip : "(no ip)",
                 (int)rssi);
    } else {
        snprintf(wifi_sub, sizeof(wifi_sub), "not connected  |  tap to scan");
    }

    // Spending row: format auto/daily caps as "$X.XX auto / $X.XX cap".
    uint32_t auto_c  = devcfg_spend_auto_max_cents();
    uint32_t daily_c = devcfg_spend_daily_cap_cents();
    char spend_sub[48];
    snprintf(spend_sub, sizeof(spend_sub),
             "$%u.%02u auto / $%u.%02u cap",
             (unsigned)(auto_c / 100u), (unsigned)(auto_c % 100u),
             (unsigned)(daily_c / 100u), (unsigned)(daily_c % 100u));

    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_wifi_sub, wifi_sub);

    if (bt_on) lv_obj_add_state   (s_bt_switch, LV_STATE_CHECKED);
    else       lv_obj_remove_state(s_bt_switch, LV_STATE_CHECKED);

    lv_slider_set_value(s_vol_slider, vol, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_vol_value, "%u", (unsigned)vol);

    lv_slider_set_value(s_bri_slider, bri, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_bri_value, "%u", (unsigned)bri);

    if (s_spend_row_value) lv_label_set_text(s_spend_row_value, spend_sub);

    lvgl_port_unlock();
}

void settings_screen_on_wifi_click(settings_wifi_click_cb_t cb) {
    s_wifi_click_cb = cb;
}
