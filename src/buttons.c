// ---------------------------------------------------------------------------
//  buttons.c — BSP_BUTTON_MAIN as push-to-talk.
//
//  See buttons.h for the architectural notes (why we don't call the BSP's
//  bsp_iot_button_create()). Summary: the front-face button is exposed by
//  the GT911 via its key register, we read that register via our existing
//  touch handle (touch.c::touch_handle), and we feed the level into
//  iot_button so we get debounce + press/release events for free.
//
//  iot_button polls our get_key_level() callback on its own task at
//  ~13 ms intervals. The GT911's cached button state is refreshed every
//  LVGL tick (~33 ms) by esp_lcd_touch_read_data() — so worst-case latency
//  on press/release is one LVGL tick + iot_button's debounce window
//  (~30 ms by default). Total ~60-70 ms, well under perceptible for PTT.
//
//  Threading: iot_button event callbacks fire on the iot_button task, but
//  creature_screen_ptt_start/stop touch LVGL widgets (subtitle text), so
//  we hop to the LVGL thread via lv_async_call before invoking them.
// ---------------------------------------------------------------------------
#include "buttons.h"
#include "creature_screen.h"
#include "touch.h"

#include "esp_log.h"
#include "esp_lcd_touch.h"
#include "iot_button.h"
#include "button_types.h"
#include "lvgl.h"

static const char *TAG = "buttons";

static button_handle_t s_main_btn = NULL;

// Reads the cached state of the GT911's home-button bit. Returns
// BUTTON_ACTIVE when the button is pressed, BUTTON_INACTIVE otherwise.
// Called from the iot_button polling task — fast, no I2C work here
// (read_data has already populated the cached status).
static uint8_t main_btn_get_level(button_driver_t *drv) {
    (void)drv;
    esp_lcd_touch_handle_t tp = touch_handle();
    if (!tp) return BUTTON_INACTIVE;
    uint8_t state = 0;
    if (esp_lcd_touch_get_button_state(tp, 0, &state) != ESP_OK) {
        return BUTTON_INACTIVE;
    }
    return state ? BUTTON_ACTIVE : BUTTON_INACTIVE;
}

static button_driver_t s_main_btn_drv = {
    .enable_power_save = false,
    .get_key_level     = main_btn_get_level,
    .enter_power_save  = NULL,
    .del               = NULL,
};

// LVGL-thread shims — the actual creature_screen_ptt_* functions can be
// called from any task per their docstring, but the subtitle update they
// do internally needs the LVGL lock, which lv_async_call hands us for free.
static void on_press_down_async(void *arg) {
    (void)arg;
    creature_screen_ptt_start();
}

static void on_press_up_async(void *arg) {
    (void)arg;
    creature_screen_ptt_stop();
}

static void on_press_down(void *btn, void *user_data) {
    (void)btn;
    (void)user_data;
    lv_async_call(on_press_down_async, NULL);
}

static void on_press_up(void *btn, void *user_data) {
    (void)btn;
    (void)user_data;
    lv_async_call(on_press_up_async, NULL);
}

esp_err_t buttons_init(void) {
    if (!touch_handle()) {
        ESP_LOGW(TAG, "touch handle unavailable; PTT button disabled");
        return ESP_OK;
    }

    const button_config_t cfg = {0};   // defaults: short=180 ms, long=1500 ms
    esp_err_t err = iot_button_create(&cfg, &s_main_btn_drv, &s_main_btn);
    if (err != ESP_OK || !s_main_btn) {
        ESP_LOGW(TAG, "iot_button_create failed: %s", esp_err_to_name(err));
        return err;
    }

    iot_button_register_cb(s_main_btn, BUTTON_PRESS_DOWN, NULL, on_press_down, NULL);
    iot_button_register_cb(s_main_btn, BUTTON_PRESS_UP,   NULL, on_press_up,   NULL);

    ESP_LOGI(TAG, "PTT bound to BSP_BUTTON_MAIN (front-face, via GT911 key bit)");
    return ESP_OK;
}
