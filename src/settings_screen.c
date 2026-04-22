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
#include "voice.h"
#include "wifi_sta.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "settings_screen";

static lv_obj_t *s_scr = NULL;

// Status bar labels (top).
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_price_label  = NULL;
static lv_obj_t *s_usdc_label   = NULL;

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

    // --- status bar ---
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

    s_price_label = lv_label_create(bar);
    lv_label_set_text(s_price_label, "");
    lv_obj_set_style_text_color(s_price_label, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(s_price_label, LV_ALIGN_TOP_RIGHT, 0, -2);

    s_usdc_label = lv_label_create(bar);
    lv_label_set_text(s_usdc_label, "");
    lv_obj_set_style_text_color(s_usdc_label, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_usdc_label, LV_ALIGN_BOTTOM_RIGHT, 0, 2);

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

    lvgl_port_unlock();

    // Seed current values.
    settings_screen_refresh();

    ESP_LOGI(TAG, "settings screen built");
    return true;
}

lv_obj_t *settings_screen(void) { return s_scr; }

void settings_screen_set_status(const char *s) {
    if (!s_status_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_label, s ? s : "");
    lvgl_port_unlock();
}

void settings_screen_set_price(const char *s) {
    if (!s_price_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_price_label, s ? s : "");
    lvgl_port_unlock();
}

void settings_screen_set_usdc(const char *s) {
    if (!s_usdc_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_usdc_label, s ? s : "");
    lvgl_port_unlock();
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
        snprintf(wifi_sub, sizeof(wifi_sub), "%s  •  %s  •  %d dBm",
                 ssid[0] ? ssid : "(unknown)",
                 ip[0] ? ip : "(no ip)",
                 (int)rssi);
    } else {
        snprintf(wifi_sub, sizeof(wifi_sub), "not connected  •  tap to scan");
    }

    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_wifi_sub, wifi_sub);

    if (bt_on) lv_obj_add_state   (s_bt_switch, LV_STATE_CHECKED);
    else       lv_obj_remove_state(s_bt_switch, LV_STATE_CHECKED);

    lv_slider_set_value(s_vol_slider, vol, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_vol_value, "%u", (unsigned)vol);

    lv_slider_set_value(s_bri_slider, bri, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_bri_value, "%u", (unsigned)bri);

    lvgl_port_unlock();
}

void settings_screen_on_wifi_click(settings_wifi_click_cb_t cb) {
    s_wifi_click_cb = cb;
}
