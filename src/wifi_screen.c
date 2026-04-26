// ---------------------------------------------------------------------------
//  Wi-Fi picker implementation.
//
//  Background tasks:
//    • scan_task — runs wifi_sta_scan() into a module-local buffer, then
//                  lv_async_call's repopulate the list widget.
//    • connect_task — runs wifi_sta_connect() with the user's SSID/password,
//                     then lv_async_call's "ok" or "fail" back to the UI.
//
//  Why dedicated tasks instead of a worker pool: there's at most one scan
//  and one connect outstanding at a time. A single xTaskCreate per action
//  is simpler than a queue, and the stack (5 KB) comes from PSRAM if
//  available — the radio/TLS peak is well under that.
//
//  Modal layering: LVGL has no explicit Z-order API beyond parent order.
//  The modal is a child of the screen and we toggle its hidden flag; when
//  visible it covers the entire screen (full SCR_W x SCR_H), eating touch
//  events on the list below.
// ---------------------------------------------------------------------------
#include "wifi_screen.h"
#include "screens_common.h"
#include "ui.h"
#include "wifi_sta.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "wifi_screen";

// --- widget handles --------------------------------------------------------
static lv_obj_t *s_scr          = NULL;
static lv_obj_t *s_status_label = NULL;
// price + usdc retired here — ticker is creature-only. Setters kept as
// no-ops for ui.c broadcast compatibility.
static lv_obj_t *s_scan_btn     = NULL;
static lv_obj_t *s_list         = NULL;
static lv_obj_t *s_hint         = NULL;

// Password modal.
static lv_obj_t *s_modal        = NULL;
static lv_obj_t *s_modal_title  = NULL;
static lv_obj_t *s_pw_input     = NULL;
static lv_obj_t *s_keyboard     = NULL;
static lv_obj_t *s_connect_btn  = NULL;
static lv_obj_t *s_cancel_btn   = NULL;

// --- state -----------------------------------------------------------------
#define WIFI_MAX_APS 16
static wifi_sta_scan_ap_t s_aps[WIFI_MAX_APS];
static size_t             s_ap_count = 0;

// Currently-focused SSID (valid only while the password modal is visible).
static char s_pending_ssid[33] = "";
static bool s_pending_is_open  = false;

static volatile bool s_scan_in_flight    = false;
static volatile bool s_connect_in_flight = false;

static wifi_screen_connected_cb_t s_on_connected = NULL;

// --- forward decls ---------------------------------------------------------
static void repopulate_list(void);
static void show_modal(const char *ssid);
static void hide_modal(void);

// --- shared close-X (see wallet_screen.c for the canonical copy) ----------
static void close_clicked(lv_event_t *e) {
    (void)e;
    // If the password modal is up, close the modal first — treat the X as
    // "cancel this interaction" rather than punting straight home. Second
    // tap lands back on the creature.
    if (s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN)) {
        hide_modal();
        return;
    }
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

// --- async hops from background task -> LVGL task --------------------------

static void async_scan_done(void *arg) {
    (void)arg;
    if (!lvgl_port_lock(0)) return;
    s_scan_in_flight = false;
    repopulate_list();
    lv_label_set_text(s_status_label, "wifi");
    lvgl_port_unlock();
}

// Payload for connect result so we can surface the SSID in the message.
typedef struct {
    bool ok;
    char ssid[33];
} connect_result_t;

static void async_connect_done(void *arg) {
    connect_result_t *r = (connect_result_t *)arg;
    if (!r) return;
    if (lvgl_port_lock(0)) {
        s_connect_in_flight = false;
        if (r->ok) {
            lv_label_set_text_fmt(s_status_label, "connected: %s", r->ssid);
            hide_modal();
            if (s_on_connected) s_on_connected();
        } else {
            lv_label_set_text_fmt(s_status_label, "connect failed: %s", r->ssid);
            // Keep the modal up so the user can retry / edit the password.
        }
        lvgl_port_unlock();
    }
    free(r);
}

// --- background tasks ------------------------------------------------------

static void scan_task(void *arg) {
    (void)arg;
    // Blocks ~2-3 s. Writes into the module buffer; the async hop copies
    // nothing since the buffer is module-local.
    s_ap_count = wifi_sta_scan(s_aps, WIFI_MAX_APS);
    ESP_LOGI(TAG, "scan found %u APs", (unsigned)s_ap_count);
    lv_async_call(async_scan_done, NULL);
    vTaskDelete(NULL);
}

typedef struct {
    char ssid[33];
    char password[65];
} connect_args_t;

static void connect_task(void *arg) {
    connect_args_t *a = (connect_args_t *)arg;
    ESP_LOGI(TAG, "connecting to %s", a->ssid);

    esp_err_t err = wifi_sta_connect(a->ssid, a->password, 15000);

    connect_result_t *r = calloc(1, sizeof(*r));
    if (r) {
        r->ok = (err == ESP_OK);
        strncpy(r->ssid, a->ssid, sizeof(r->ssid) - 1);
    }
    lv_async_call(async_connect_done, r);

    // Zero the password out of RAM before freeing.
    memset(a, 0, sizeof(*a));
    free(a);
    vTaskDelete(NULL);
}

// --- LVGL event handlers (all run under lvgl_port mutex) -------------------

static void scan_clicked(lv_event_t *e) {
    (void)e;
    if (s_scan_in_flight) return;
    s_scan_in_flight = true;
    lv_label_set_text(s_status_label, "scanning...");
    xTaskCreate(scan_task, "wifi_scan", 8192, NULL, 5, NULL);
}

// Passed as user_data on the row's CLICKED event. Points into s_aps, which
// lives forever once initialised.
static void row_clicked(lv_event_t *e) {
    const wifi_sta_scan_ap_t *ap = (const wifi_sta_scan_ap_t *)lv_event_get_user_data(e);
    if (!ap) return;

    if (ap->auth_open) {
        // No password required — kick the connect straight away.
        if (s_connect_in_flight) return;
        connect_args_t *a = calloc(1, sizeof(*a));
        if (!a) return;
        strncpy(a->ssid, ap->ssid, sizeof(a->ssid) - 1);
        s_connect_in_flight = true;
        lv_label_set_text_fmt(s_status_label, "connecting to %s", ap->ssid);
        xTaskCreate(connect_task, "wifi_conn", 6144, a, 5, NULL);
    } else {
        strncpy(s_pending_ssid, ap->ssid, sizeof(s_pending_ssid) - 1);
        s_pending_ssid[sizeof(s_pending_ssid) - 1] = 0;
        s_pending_is_open = false;
        show_modal(ap->ssid);
    }
}

static void connect_clicked(lv_event_t *e) {
    (void)e;
    if (s_connect_in_flight) return;
    const char *pw = lv_textarea_get_text(s_pw_input);
    if (!pw) pw = "";
    connect_args_t *a = calloc(1, sizeof(*a));
    if (!a) return;
    strncpy(a->ssid, s_pending_ssid, sizeof(a->ssid) - 1);
    strncpy(a->password, pw, sizeof(a->password) - 1);
    s_connect_in_flight = true;
    lv_label_set_text_fmt(s_status_label, "connecting to %s", s_pending_ssid);
    xTaskCreate(connect_task, "wifi_conn", 6144, a, 5, NULL);
}

static void cancel_clicked(lv_event_t *e) {
    (void)e;
    hide_modal();
}

static void keyboard_ready(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        // Green check on the keyboard = "connect"
        connect_clicked(e);
    } else if (code == LV_EVENT_CANCEL) {
        cancel_clicked(e);
    }
}

// --- list + modal helpers --------------------------------------------------

static void repopulate_list(void) {
    lv_obj_clean(s_list);
    if (s_ap_count == 0) {
        lv_obj_t *empty = lv_list_add_text(s_list, "No networks found");
        lv_obj_set_style_text_color(empty, SCR_COLOR_DIM, LV_PART_MAIN);
        return;
    }
    for (size_t i = 0; i < s_ap_count; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%s%s  %d dBm",
                 s_aps[i].ssid,
                 s_aps[i].auth_open ? "" : " *",
                 (int)s_aps[i].rssi);
        // lv_list_add_button gives us a clickable row. Passing &s_aps[i]
        // as user_data so the click handler knows which one was tapped
        // without a search.
        lv_obj_t *row = lv_list_add_button(s_list, NULL, line);
        lv_obj_set_style_bg_color(row, SCR_COLOR_PANEL, LV_PART_MAIN);
        lv_obj_set_style_text_color(row, SCR_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, &s_aps[i]);
    }
}

static void show_modal(const char *ssid) {
    if (!s_modal) return;
    lv_label_set_text_fmt(s_modal_title, "Connect to %s", ssid);
    lv_textarea_set_text(s_pw_input, "");
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    // Bring modal to front (LVGL renders children in creation order; move
    // to foreground for safety if we later add post-creation siblings).
    lv_obj_move_foreground(s_modal);
}

static void hide_modal(void) {
    if (!s_modal) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
    // Wipe the textarea so a shoulder-surfer doesn't read the last password.
    lv_textarea_set_text(s_pw_input, "");
}

// --- public API ------------------------------------------------------------

bool wifi_screen_init(void) {
    if (s_scr) return true;
    if (!lvgl_port_lock(0)) { ESP_LOGE(TAG, "lvgl_port_lock failed"); return false; }

    s_scr = lv_obj_create(NULL);
    scr_apply_bg(s_scr);

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
    lv_label_set_text(s_status_label, "wifi");
    lv_obj_set_style_text_color(s_status_label, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    add_close_button(bar);

    // --- scan button ---
    s_scan_btn = lv_button_create(s_scr);
    lv_obj_set_size(s_scan_btn, SCR_W - 24, 40);
    lv_obj_align(s_scan_btn, LV_ALIGN_TOP_LEFT, 12, STATUS_BAR_H + 8);
    lv_obj_set_style_bg_color(s_scan_btn, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(s_scan_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(s_scan_btn, scan_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(s_scan_btn);
    lv_label_set_text(btn_label, "Scan");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(btn_label);

    // --- AP list ---
    s_list = lv_list_create(s_scr);
    lv_obj_set_size(s_list, SCR_W - 20, SCR_H - (STATUS_BAR_H + 60) - 30);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 10, STATUS_BAR_H + 56);
    lv_obj_set_style_bg_color(s_list, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_list, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_list, SCR_COLOR_TEXT, LV_PART_MAIN);

    // --- footer hint ---
    s_hint = lv_label_create(s_scr);
    lv_label_set_text(s_hint, "swipe for settings");
    lv_obj_set_style_text_color(s_hint, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    // --- password modal --------------------------------------------------
    // Full-screen opaque cover so the list below is visually + input-wise
    // masked out.
    s_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_modal, SCR_W, SCR_H);
    lv_obj_align(s_modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_modal, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_modal, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_modal, 8, LV_PART_MAIN);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    s_modal_title = lv_label_create(s_modal);
    lv_label_set_text(s_modal_title, "Connect");
    lv_obj_set_style_text_color(s_modal_title, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_modal_title, LV_ALIGN_TOP_LEFT, 4, 4);

    s_pw_input = lv_textarea_create(s_modal);
    lv_obj_set_size(s_pw_input, SCR_W - 32, 40);
    lv_obj_align(s_pw_input, LV_ALIGN_TOP_LEFT, 4, 32);
    lv_textarea_set_password_mode(s_pw_input, true);
    lv_textarea_set_one_line(s_pw_input, true);
    lv_textarea_set_placeholder_text(s_pw_input, "password");
    lv_obj_set_style_bg_color(s_pw_input, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pw_input, SCR_COLOR_TEXT, LV_PART_MAIN);

    // Buttons row (above the keyboard so they stay visible while typing).
    s_cancel_btn = lv_button_create(s_modal);
    lv_obj_set_size(s_cancel_btn, 90, 36);
    lv_obj_align(s_cancel_btn, LV_ALIGN_TOP_LEFT, 8, 84);
    lv_obj_set_style_bg_color(s_cancel_btn, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_add_event_cb(s_cancel_btn, cancel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(s_cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(cl);

    s_connect_btn = lv_button_create(s_modal);
    lv_obj_set_size(s_connect_btn, 90, 36);
    lv_obj_align(s_connect_btn, LV_ALIGN_TOP_RIGHT, -8, 84);
    lv_obj_set_style_bg_color(s_connect_btn, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_add_event_cb(s_connect_btn, connect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = lv_label_create(s_connect_btn);
    lv_label_set_text(ol, "Connect");
    lv_obj_set_style_text_color(ol, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(ol);

    // On-screen keyboard. Bound to the password textarea so key taps go
    // straight in; READY/CANCEL codes forward to connect/cancel.
    s_keyboard = lv_keyboard_create(s_modal);
    lv_keyboard_set_textarea(s_keyboard, s_pw_input);
    lv_obj_set_size(s_keyboard, SCR_W, 180);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_keyboard, keyboard_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_ready, LV_EVENT_CANCEL, NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "wifi screen built");
    return true;
}

lv_obj_t *wifi_screen(void) { return s_scr; }

void wifi_screen_set_status(const char *s) {
    if (!s_status_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_label, s ? s : "");
    lvgl_port_unlock();
}

void wifi_screen_set_price(const char *s) {
    // No-op: ticker is creature-only now. See wallet_screen_set_price().
    (void)s;
}

void wifi_screen_set_usdc(const char *s) {
    // Ditto.
    (void)s;
}

void wifi_screen_kick_scan(void) {
    if (s_scan_in_flight) return;
    s_scan_in_flight = true;
    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_status_label, "scanning...");
        lvgl_port_unlock();
    }
    xTaskCreate(scan_task, "wifi_scan", 8192, NULL, 5, NULL);
}

void wifi_screen_on_connected(wifi_screen_connected_cb_t cb) {
    s_on_connected = cb;
}
