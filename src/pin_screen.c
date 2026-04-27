// ---------------------------------------------------------------------------
//  PIN keypad screen. See pin_screen.h.
//
//  Layout (240×320 portrait):
//     y=  8  ENTER PIN
//     y= 40  ● ● ● _ _ _    (dots: filled = entered, empty = pending)
//     y= 90  3×4 keypad: 1 2 3 / 4 5 6 / 7 8 9 / ⌫ 0 ↵
//     y=300  status line (errors / hints)
//
//  Cancel (×) lives in the top-right corner so the user can back out
//  during setup (boot-time can choose to ignore cancel by re-opening).
// ---------------------------------------------------------------------------
#include "pin_screen.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "screens_common.h"

static const char *TAG = "pin_screen";

#define PIN_BUF_MAX 33   // pin.h's PIN_MAX_LEN (32) + NUL

typedef struct {
    lv_obj_t        *scr;
    lv_obj_t        *prev_scr;        // restored on close
    lv_obj_t        *dots[16];        // up to 16 dot indicators
    lv_obj_t        *status_lbl;
    pin_screen_cb_t  cb;
    void            *user;
    int              min_len;
    int              max_len;
    int              len;             // currently entered length
    char             buf[PIN_BUF_MAX];
    bool             open;
    bool             touch_active;    // true between PRESSED and RELEASED
    int64_t          unlock_at_us;    // earliest timestamp the next press may fire
} pin_ctx_t;

// CST328 reports release jitter as multiple PRESSED/RELEASED cycles per
// continuous physical touch. A debounce window alone isn't enough — long
// holds let multiple presses through after the cooldown. Instead we lock
// on the first PRESSED of a touch and don't unlock until we see a real
// RELEASED followed by SETTLE_US of quiet. Subsequent jitter events
// during the same hold are ignored, regardless of how long the hold is.
#define PIN_BTN_SETTLE_US 200000        // 200 ms of no events = touch ended

static pin_ctx_t s_ctx = {0};

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
static void redraw_dots(void) {
    for (int i = 0; i < s_ctx.max_len && i < 16; i++) {
        lv_color_t c = (i < s_ctx.len) ? SCR_COLOR_ACCENT_HI : SCR_COLOR_DIM;
        lv_obj_set_style_bg_color(s_ctx.dots[i], c, LV_PART_MAIN);
    }
}

void pin_screen_clear_input(void) {
    if (!s_ctx.open) return;
    s_ctx.len = 0;
    s_ctx.buf[0] = '\0';
    redraw_dots();
}

void pin_screen_set_status(const char *text) {
    if (!s_ctx.open || !s_ctx.status_lbl) return;
    lv_label_set_text(s_ctx.status_lbl, text ? text : "");
}

// ---------------------------------------------------------------------------
// Button event handlers
// ---------------------------------------------------------------------------
// True iff this PRESSED event is the first one of a fresh physical touch.
// Sets touch_active so subsequent jitter PRESSED events during the same
// hold are dropped. RELEASED arms a settle timer (unlock_at_us); until
// SETTLE_US has passed since the last release, no new press counts.
static bool press_acquire(void) {
    int64_t now = esp_timer_get_time();
    if (s_ctx.touch_active)               return false;   // jitter mid-hold
    if (now < s_ctx.unlock_at_us)         return false;   // still in settle window
    s_ctx.touch_active = true;
    return true;
}

static void release_arm_settle(void) {
    s_ctx.touch_active = false;
    s_ctx.unlock_at_us = esp_timer_get_time() + PIN_BTN_SETTLE_US;
}

static void on_btn_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        release_arm_settle();
        return;
    }
    if (code != LV_EVENT_PRESSED) return;
    if (!press_acquire()) return;

    // Dispatch by user_data tag set up in build_ui:
    //   0..9 → digit, 10 → backspace, 11 → OK, 12 → cancel
    int tag = (int)(intptr_t)lv_event_get_user_data(e);
    if (tag >= 0 && tag <= 9) {
        if (s_ctx.len < s_ctx.max_len) {
            s_ctx.buf[s_ctx.len++] = (char)('0' + tag);
            s_ctx.buf[s_ctx.len]   = '\0';
            redraw_dots();
        }
    } else if (tag == 10) {
        if (s_ctx.len > 0) {
            s_ctx.len--;
            s_ctx.buf[s_ctx.len] = '\0';
            redraw_dots();
        }
    } else if (tag == 11) {
        if (s_ctx.len < s_ctx.min_len) {
            pin_screen_set_status("PIN too short");
            return;
        }
        pin_screen_cb_t cb = s_ctx.cb;
        void           *user = s_ctx.user;
        char            pin[PIN_BUF_MAX];
        strlcpy(pin, s_ctx.buf, sizeof pin);
        if (cb) cb(PIN_SCR_OK, pin, user);
    } else if (tag == 12) {
        pin_screen_cb_t cb = s_ctx.cb;
        void           *user = s_ctx.user;
        if (cb) cb(PIN_SCR_CANCEL, NULL, user);
    }
}

// ---------------------------------------------------------------------------
// Build / teardown
// ---------------------------------------------------------------------------
// Tag values map to the unified on_btn_event dispatcher:
//   0..9 = digit 0..9
//   10   = backspace
//   11   = OK
//   12   = cancel (×)
static lv_obj_t *make_button(lv_obj_t *parent, const char *label,
                             int x, int y, int w, int h, int tag)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_center(lbl);

    // Hook BOTH PRESSED and RELEASED on a single dispatcher. on_btn_event
    // arbitrates which events actually act based on the touch lock state.
    lv_obj_add_event_cb(btn, on_btn_event, LV_EVENT_PRESSED,    (void *)(intptr_t)tag);
    lv_obj_add_event_cb(btn, on_btn_event, LV_EVENT_RELEASED,   (void *)(intptr_t)tag);
    lv_obj_add_event_cb(btn, on_btn_event, LV_EVENT_PRESS_LOST, (void *)(intptr_t)tag);
    return btn;
}

static void build_ui(void) {
    s_ctx.prev_scr = lv_scr_act();
    s_ctx.scr = lv_obj_create(NULL);
    scr_apply_bg(s_ctx.scr);

    // Title
    lv_obj_t *title = lv_label_create(s_ctx.scr);
    lv_label_set_text(title, "ENTER PIN");
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    // Cancel button (top-right) — tag 12
    lv_obj_t *cancel = make_button(s_ctx.scr, LV_SYMBOL_CLOSE,
                                   SCR_W - 50, 6, 44, 32, 12);
    lv_obj_set_style_border_color(cancel, SCR_COLOR_DIM, LV_PART_MAIN);

    // Dots row — center the (max_len) dots, 18 px each, 6 px gap
    int dot_count = s_ctx.max_len > 16 ? 16 : s_ctx.max_len;
    int dot_w     = 16;
    int gap       = 8;
    int total_w   = dot_count * dot_w + (dot_count - 1) * gap;
    int start_x   = (SCR_W - total_w) / 2;
    for (int i = 0; i < dot_count; i++) {
        lv_obj_t *d = lv_obj_create(s_ctx.scr);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(d, dot_w, dot_w);
        lv_obj_set_pos(d, start_x + i * (dot_w + gap), 50);
        lv_obj_set_style_radius(d, dot_w / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(d, SCR_COLOR_DIM, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
        s_ctx.dots[i] = d;
    }

    // Keypad: 3 cols, 4 rows. Buttons 60×44 with 8 px gap.
    const int btn_w = 60, btn_h = 44, hg = 12, vg = 10;
    const int grid_w = 3 * btn_w + 2 * hg;
    const int grid_x = (SCR_W - grid_w) / 2;
    const int grid_y = 100;

    static const char *DIGITS[] = { "1","2","3","4","5","6","7","8","9" };
    for (int i = 0; i < 9; i++) {
        int row = i / 3;
        int col = i % 3;
        int x = grid_x + col * (btn_w + hg);
        int y = grid_y + row * (btn_h + vg);
        make_button(s_ctx.scr, DIGITS[i], x, y, btn_w, btn_h, /*tag=*/i + 1);
    }
    // Row 4: backspace (tag 10), 0 (tag 0), OK (tag 11)
    int row4_y = grid_y + 3 * (btn_h + vg);
    make_button(s_ctx.scr, LV_SYMBOL_BACKSPACE,
                grid_x + 0 * (btn_w + hg), row4_y, btn_w, btn_h, 10);
    make_button(s_ctx.scr, "0",
                grid_x + 1 * (btn_w + hg), row4_y, btn_w, btn_h, 0);
    lv_obj_t *ok_btn = make_button(s_ctx.scr, LV_SYMBOL_OK,
                                   grid_x + 2 * (btn_w + hg), row4_y, btn_w, btn_h, 11);
    lv_obj_set_style_border_color(ok_btn, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);

    // Status line
    s_ctx.status_lbl = lv_label_create(s_ctx.scr);
    lv_label_set_text(s_ctx.status_lbl, "");
    lv_obj_set_style_text_color(s_ctx.status_lbl, SCR_COLOR_WARN, LV_PART_MAIN);
    lv_obj_align(s_ctx.status_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    redraw_dots();

    lv_scr_load(s_ctx.scr);
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------
void pin_screen_open(int min_len, int max_len,
                     pin_screen_cb_t cb, void *user)
{
    if (s_ctx.open) {
        ESP_LOGW(TAG, "pin_screen_open while already open — ignoring");
        return;
    }
    if (min_len < 1) min_len = 4;
    if (max_len > 16) max_len = 16;
    if (max_len < min_len) max_len = min_len;

    memset(&s_ctx, 0, sizeof s_ctx);
    s_ctx.min_len = min_len;
    s_ctx.max_len = max_len;
    s_ctx.cb      = cb;
    s_ctx.user    = user;
    s_ctx.open    = true;

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        s_ctx.open = false;
        return;
    }
    build_ui();
    lvgl_port_unlock();
}

void pin_screen_close(void) {
    if (!s_ctx.open) return;
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "pin_screen_close: lock failed");
        return;
    }
    if (s_ctx.prev_scr) lv_scr_load(s_ctx.prev_scr);
    if (s_ctx.scr)      lv_obj_del(s_ctx.scr);
    lvgl_port_unlock();
    memset(&s_ctx, 0, sizeof s_ctx);
}
