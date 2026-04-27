// ---------------------------------------------------------------------------
//  On-device first-run wizard. See onboard_screen.h.
//
//  Architecture:
//    - One LVGL screen owns four child panels (welcome / wifi_list /
//      wifi_password / connecting). Each is a full-screen container, all
//      hidden except the active one. Transitions just toggle visibility.
//    - PIN entry hands off to pin_screen_open (which loads its own LVGL
//      screen), then reloads our wizard screen on completion.
//    - wifi_sta_scan and wifi_sta_connect block for seconds; they run on a
//      worker task so the LVGL task stays responsive (touch events still
//      flow). Worker → LVGL updates are marshaled via lv_async_call.
//    - The public entry point (onboard_screen_run) blocks on a binary
//      semaphore that's signaled in the DONE / FAILED states.
// ---------------------------------------------------------------------------
#include "onboard_screen.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "lvgl.h"

#include "pin.h"
#include "pin_screen.h"
#include "screens_common.h"
#include "wifi_sta.h"
#include "wizard.h"

static const char *TAG = "onboard";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef enum {
    OB_WELCOME,
    OB_WIFI_LIST,
    OB_WIFI_PASSWORD,
    OB_WIFI_CONNECTING,
    OB_PIN_FIRST,
    OB_PIN_CONFIRM,
    OB_DONE,
    OB_FAILED,
} ob_state_t;

#define OB_MAX_APS  16

static SemaphoreHandle_t s_done_sem = NULL;
static volatile bool      s_success = false;
static ob_state_t         s_state    = OB_WELCOME;

// Captured form values across screens. Cleared in onboard_screen_run before
// reuse so a re-entry starts fresh.
static char s_chosen_ssid[33]  = {0};
static char s_chosen_pw[65]    = {0};
static char s_first_pin[33]    = {0};
static bool s_chosen_open      = false;   // true if the picked AP is open

// LVGL widgets. Built once in build_screen, referenced by the state-
// transition helpers.
static lv_obj_t *s_scr            = NULL;
static lv_obj_t *s_panel_welcome  = NULL;
static lv_obj_t *s_panel_wifi     = NULL;
static lv_obj_t *s_panel_pw       = NULL;
static lv_obj_t *s_panel_connect  = NULL;
static lv_obj_t *s_wifi_list      = NULL;
static lv_obj_t *s_wifi_status    = NULL;
static lv_obj_t *s_pw_title       = NULL;
static lv_obj_t *s_pw_input       = NULL;
static lv_obj_t *s_pw_keyboard    = NULL;
static lv_obj_t *s_connect_status = NULL;

// Live AP buffer for the list. Each row carries a pointer into here as its
// user_data; we re-scan into the same array so the pointers stay valid.
static wifi_sta_scan_ap_t s_aps[OB_MAX_APS];
static size_t             s_ap_count = 0;

// Forward decls
static void show_panel(lv_obj_t *which);
static void enter_state(ob_state_t s);
static void on_wifi_row_clicked(lv_event_t *e);
static void on_pw_connect_clicked(lv_event_t *e);
static void on_pw_cancel_clicked(lv_event_t *e);
static void on_welcome_begin(lv_event_t *e);
static void scan_async_kick(void);
static void connect_async_kick(void);
static void pin_first_cb(pin_screen_result_t r, const char *pin, void *user);
static void pin_confirm_cb(pin_screen_result_t r, const char *pin, void *user);

// ---------------------------------------------------------------------------
// Panel visibility — one panel shown at a time
// ---------------------------------------------------------------------------
static void show_panel(lv_obj_t *which) {
    lv_obj_t *all[] = { s_panel_welcome, s_panel_wifi, s_panel_pw, s_panel_connect };
    for (size_t i = 0; i < sizeof(all)/sizeof(*all); i++) {
        if (all[i] == NULL) continue;
        if (all[i] == which) lv_obj_remove_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_add_flag   (all[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
static void enter_state(ob_state_t next) {
    s_state = next;
    ESP_LOGI(TAG, "state -> %d", (int)next);

    switch (next) {
        case OB_WELCOME:
            show_panel(s_panel_welcome);
            break;

        case OB_WIFI_LIST:
            show_panel(s_panel_wifi);
            lv_label_set_text(s_wifi_status, "scanning...");
            lv_obj_clean(s_wifi_list);
            scan_async_kick();
            break;

        case OB_WIFI_PASSWORD:
            show_panel(s_panel_pw);
            lv_label_set_text_fmt(s_pw_title, "Password for\n%s", s_chosen_ssid);
            lv_textarea_set_text(s_pw_input, "");
            // Focus the textarea so the keyboard's first tap goes there.
            lv_obj_add_state(s_pw_input, LV_STATE_FOCUSED);
            break;

        case OB_WIFI_CONNECTING:
            show_panel(s_panel_connect);
            lv_label_set_text_fmt(s_connect_status, "joining\n%s ...", s_chosen_ssid);
            connect_async_kick();
            break;

        case OB_PIN_FIRST:
            // Hand the screen over to pin_screen — it loads its own LVGL
            // screen and we'll get our wizard back when its callback fires.
            pin_screen_open(PIN_MIN_LEN, 6, pin_first_cb, NULL);
            break;

        case OB_PIN_CONFIRM:
            pin_screen_set_status("confirm PIN");
            pin_screen_clear_input();
            // pin_screen is still open from the previous step; we just
            // adjust its status label and wait for the second submit.
            break;

        case OB_DONE:
            s_success = true;
            xSemaphoreGive(s_done_sem);
            break;

        case OB_FAILED:
            s_success = false;
            xSemaphoreGive(s_done_sem);
            break;
    }
}

// ---------------------------------------------------------------------------
// Async worker for wifi_sta_scan
// ---------------------------------------------------------------------------
static void scan_finished_lvgl(void *arg) {
    (void)arg;
    if (!lvgl_port_lock(0)) return;
    lv_obj_clean(s_wifi_list);
    if (s_ap_count == 0) {
        lv_obj_t *empty = lv_list_add_text(s_wifi_list, "No networks found");
        lv_obj_set_style_text_color(empty, SCR_COLOR_DIM, LV_PART_MAIN);
        lv_label_set_text(s_wifi_status, "no networks — tap rescan");
    } else {
        for (size_t i = 0; i < s_ap_count; i++) {
            char line[64];
            snprintf(line, sizeof line, "%s  %d dBm%s",
                     s_aps[i].ssid, (int)s_aps[i].rssi,
                     s_aps[i].auth_open ? "" : "  *");
            lv_obj_t *row = lv_list_add_button(s_wifi_list, NULL, line);
            lv_obj_set_style_bg_color  (row, SCR_COLOR_PANEL, LV_PART_MAIN);
            lv_obj_set_style_text_color(row, SCR_COLOR_TEXT,  LV_PART_MAIN);
            lv_obj_add_event_cb(row, on_wifi_row_clicked, LV_EVENT_CLICKED, &s_aps[i]);
        }
        lv_label_set_text(s_wifi_status, "tap a network");
    }
    lvgl_port_unlock();
}

static void scan_task(void *arg) {
    (void)arg;
    s_ap_count = wifi_sta_scan(s_aps, OB_MAX_APS);
    lv_async_call(scan_finished_lvgl, NULL);
    vTaskDelete(NULL);
}

static void scan_async_kick(void) {
    if (xTaskCreatePinnedToCore(scan_task, "ob_scan", 4096, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "scan task create failed");
    }
}

// ---------------------------------------------------------------------------
// Async worker for wifi_sta_connect
// ---------------------------------------------------------------------------
static void connect_failed_lvgl(void *arg) {
    (void)arg;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_connect_status, "couldn't join\ntap to retry");
    // Allow tap to go back to password panel.
    lv_obj_add_flag(s_panel_connect, LV_OBJ_FLAG_CLICKABLE);
    lvgl_port_unlock();
}

static void connect_succeeded_lvgl(void *arg) {
    (void)arg;
    enter_state(OB_PIN_FIRST);
}

static void connect_task(void *arg) {
    (void)arg;
    esp_err_t err = wifi_sta_connect(s_chosen_ssid,
                                     s_chosen_open ? "" : s_chosen_pw,
                                     20000);
    if (err == ESP_OK) {
        lv_async_call(connect_succeeded_lvgl, NULL);
    } else {
        ESP_LOGW(TAG, "wifi_sta_connect: %s", esp_err_to_name(err));
        lv_async_call(connect_failed_lvgl, NULL);
    }
    vTaskDelete(NULL);
}

static void connect_async_kick(void) {
    lv_obj_remove_flag(s_panel_connect, LV_OBJ_FLAG_CLICKABLE);
    if (xTaskCreatePinnedToCore(connect_task, "ob_conn", 6144, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "connect task create failed");
    }
}

static void on_connect_panel_tap(lv_event_t *e) {
    (void)e;
    // Only meaningful after a connect failure (we re-enable the click flag
    // when the failure status is shown). Re-enter the password panel so the
    // user can fix the password.
    if (s_state == OB_WIFI_CONNECTING) {
        enter_state(OB_WIFI_PASSWORD);
    }
}

// ---------------------------------------------------------------------------
// Click handlers
// ---------------------------------------------------------------------------
static void on_welcome_begin(lv_event_t *e) {
    (void)e;
    enter_state(OB_WIFI_LIST);
}

static void on_wifi_row_clicked(lv_event_t *e) {
    const wifi_sta_scan_ap_t *ap = (const wifi_sta_scan_ap_t *)lv_event_get_user_data(e);
    if (!ap) return;
    strlcpy(s_chosen_ssid, ap->ssid, sizeof s_chosen_ssid);
    s_chosen_open = ap->auth_open != 0;
    if (s_chosen_open) {
        // No password needed — straight to connecting.
        s_chosen_pw[0] = '\0';
        enter_state(OB_WIFI_CONNECTING);
    } else {
        enter_state(OB_WIFI_PASSWORD);
    }
}

static void on_pw_connect_clicked(lv_event_t *e) {
    (void)e;
    const char *typed = lv_textarea_get_text(s_pw_input);
    strlcpy(s_chosen_pw, typed ? typed : "", sizeof s_chosen_pw);
    enter_state(OB_WIFI_CONNECTING);
}

static void on_pw_cancel_clicked(lv_event_t *e) {
    (void)e;
    enter_state(OB_WIFI_LIST);
}

static void on_pw_keyboard_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)  { on_pw_connect_clicked(NULL); }
    if (code == LV_EVENT_CANCEL) { on_pw_cancel_clicked(NULL);  }
}

// ---------------------------------------------------------------------------
// PIN setup callbacks (two passes — first then confirm)
// ---------------------------------------------------------------------------
static void pin_first_cb(pin_screen_result_t r, const char *pin, void *user) {
    (void)user;
    if (r == PIN_SCR_CANCEL) {
        // No cancel from the wizard — re-show the keypad with a hint.
        pin_screen_set_status("PIN required");
        return;
    }
    if (!pin || strlen(pin) < PIN_MIN_LEN) {
        pin_screen_set_status("too short");
        pin_screen_clear_input();
        return;
    }
    strlcpy(s_first_pin, pin, sizeof s_first_pin);
    // Swap the callback for the confirm pass and ask the user to repeat.
    pin_screen_close();
    pin_screen_open(PIN_MIN_LEN, 6, pin_confirm_cb, NULL);
    pin_screen_set_status("confirm PIN");
    s_state = OB_PIN_CONFIRM;
}

static void pin_confirm_cb(pin_screen_result_t r, const char *pin, void *user) {
    (void)user;
    if (r == PIN_SCR_CANCEL) {
        pin_screen_set_status("confirm PIN");
        return;
    }
    if (!pin || strcmp(pin, s_first_pin) != 0) {
        pin_screen_set_status("mismatch — start over");
        pin_screen_clear_input();
        // Throw away the first PIN, restart from scratch on the next OK.
        memset(s_first_pin, 0, sizeof s_first_pin);
        pin_screen_close();
        pin_screen_open(PIN_MIN_LEN, 6, pin_first_cb, NULL);
        s_state = OB_PIN_FIRST;
        return;
    }

    // Match. Build the form, hand it to the existing wizard_apply_form
    // (which seals a fresh seed under this PIN, persists the Wi-Fi creds
    // into devcfg, and marks the wizard done).
    wizard_form_t form = {0};
    strlcpy(form.ssid,         s_chosen_ssid, sizeof form.ssid);
    strlcpy(form.password,     s_chosen_pw,   sizeof form.password);
    strlcpy(form.pin,          s_first_pin,   sizeof form.pin);
    // Owner pubkey: skip for now (Phase 5 / recovery dApp will populate).
    // wizard_apply_form skips the owner-pubkey persistence when the field
    // is empty.
    esp_err_t err = wizard_apply_form(&form);
    memset(s_first_pin, 0, sizeof s_first_pin);
    memset(s_chosen_pw, 0, sizeof s_chosen_pw);

    pin_screen_close();
    if (err == ESP_OK) {
        enter_state(OB_DONE);
    } else {
        ESP_LOGE(TAG, "wizard_apply_form: %s", esp_err_to_name(err));
        enter_state(OB_FAILED);
    }
}

// ---------------------------------------------------------------------------
// Build the LVGL UI
// ---------------------------------------------------------------------------
static lv_obj_t *make_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(p, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa  (p, LV_OPA_COVER,  LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static void build_welcome_panel(void) {
    s_panel_welcome = make_panel(s_scr);

    lv_obj_t *title = lv_label_create(s_panel_welcome);
    lv_label_set_text(title, "DAEMON");
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 64);

    lv_obj_t *tag = lv_label_create(s_panel_welcome);
    lv_label_set_text(tag, "first-run setup");
    lv_obj_set_style_text_color(tag, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(tag, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *body = lv_label_create(s_panel_welcome);
    lv_label_set_text(body,
        "We'll set up:\n\n"
        "  1. Wi-Fi\n"
        "  2. PIN\n\n"
        "Tap to begin.");
    lv_obj_set_style_text_color(body, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_width(body, SCR_W - 40);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 30, 140);

    lv_obj_t *btn = lv_button_create(s_panel_welcome);
    lv_obj_set_size(btn, SCR_W - 60, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_bg_color(btn, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_welcome_begin, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Begin");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_center(bl);
}

static void build_wifi_panel(void) {
    s_panel_wifi = make_panel(s_scr);

    lv_obj_t *title = lv_label_create(s_panel_wifi);
    lv_label_set_text(title, "1 / 2  Wi-Fi");
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

    s_wifi_status = lv_label_create(s_panel_wifi);
    lv_label_set_text(s_wifi_status, "scanning...");
    lv_obj_set_style_text_color(s_wifi_status, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_wifi_status, LV_ALIGN_TOP_LEFT, 12, 40);

    s_wifi_list = lv_list_create(s_panel_wifi);
    lv_obj_set_size(s_wifi_list, SCR_W - 24, SCR_H - 110);
    lv_obj_align(s_wifi_list, LV_ALIGN_TOP_LEFT, 12, 70);
    lv_obj_set_style_bg_color (s_wifi_list, SCR_COLOR_PANEL,    LV_PART_MAIN);
    lv_obj_set_style_bg_opa   (s_wifi_list, LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_color(s_wifi_list, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wifi_list, 1,                 LV_PART_MAIN);
    lv_obj_set_style_radius  (s_wifi_list, 10,                    LV_PART_MAIN);
    lv_obj_set_style_pad_all (s_wifi_list, 4,                     LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wifi_list, SCR_COLOR_TEXT,     LV_PART_MAIN);
}

static void build_pw_panel(void) {
    s_panel_pw = make_panel(s_scr);

    s_pw_title = lv_label_create(s_panel_pw);
    lv_label_set_text(s_pw_title, "Password");
    lv_obj_set_style_text_color(s_pw_title, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_set_width(s_pw_title, SCR_W - 24);
    lv_obj_align(s_pw_title, LV_ALIGN_TOP_LEFT, 12, 8);

    s_pw_input = lv_textarea_create(s_panel_pw);
    lv_obj_set_size(s_pw_input, SCR_W - 24, 36);
    lv_obj_align(s_pw_input, LV_ALIGN_TOP_LEFT, 12, 50);
    lv_textarea_set_password_mode(s_pw_input, true);
    lv_textarea_set_one_line(s_pw_input, true);
    lv_textarea_set_placeholder_text(s_pw_input, "password");
    lv_obj_set_style_bg_color(s_pw_input, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pw_input, SCR_COLOR_TEXT, LV_PART_MAIN);

    lv_obj_t *cancel_btn = lv_button_create(s_panel_pw);
    lv_obj_set_size(cancel_btn, 80, 32);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 12, 96);
    lv_obj_set_style_bg_color(cancel_btn, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, on_pw_cancel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Back");
    lv_obj_set_style_text_color(cl, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(cl);

    lv_obj_t *connect_btn = lv_button_create(s_panel_pw);
    lv_obj_set_size(connect_btn, 100, 32);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_RIGHT, -12, 96);
    lv_obj_set_style_bg_color(connect_btn, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_add_event_cb(connect_btn, on_pw_connect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = lv_label_create(connect_btn);
    lv_label_set_text(ol, "Connect");
    lv_obj_set_style_text_color(ol, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_center(ol);

    s_pw_keyboard = lv_keyboard_create(s_panel_pw);
    lv_keyboard_set_textarea(s_pw_keyboard, s_pw_input);
    lv_obj_set_size(s_pw_keyboard, SCR_W, 175);
    lv_obj_align(s_pw_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_pw_keyboard, on_pw_keyboard_event, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_pw_keyboard, on_pw_keyboard_event, LV_EVENT_CANCEL, NULL);
}

static void build_connect_panel(void) {
    s_panel_connect = make_panel(s_scr);
    lv_obj_add_flag(s_panel_connect, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_panel_connect, on_connect_panel_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t *spin = lv_spinner_create(s_panel_connect);
    lv_obj_set_size(spin, 64, 64);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, -36);
    lv_obj_set_style_arc_color(spin, SCR_COLOR_ACCENT, LV_PART_INDICATOR);

    s_connect_status = lv_label_create(s_panel_connect);
    lv_label_set_text(s_connect_status, "joining...");
    lv_obj_set_style_text_color(s_connect_status, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_width(s_connect_status, SCR_W - 24);
    lv_obj_set_style_text_align(s_connect_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_connect_status, LV_ALIGN_CENTER, 0, 48);
}

static void build_screen(void) {
    s_scr = lv_obj_create(NULL);
    scr_apply_bg(s_scr);

    build_welcome_panel();
    build_wifi_panel();
    build_pw_panel();
    build_connect_panel();

    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// Public entry
// ---------------------------------------------------------------------------
bool onboard_screen_run(void) {
    s_done_sem = xSemaphoreCreateBinary();
    if (!s_done_sem) return false;
    s_success = false;
    s_state   = OB_WELCOME;
    memset(s_chosen_ssid, 0, sizeof s_chosen_ssid);
    memset(s_chosen_pw,   0, sizeof s_chosen_pw);
    memset(s_first_pin,   0, sizeof s_first_pin);

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        vSemaphoreDelete(s_done_sem); s_done_sem = NULL;
        return false;
    }
    build_screen();
    enter_state(OB_WELCOME);
    lvgl_port_unlock();

    xSemaphoreTake(s_done_sem, portMAX_DELAY);
    bool ok = s_success;
    vSemaphoreDelete(s_done_sem);
    s_done_sem = NULL;
    return ok;
}
