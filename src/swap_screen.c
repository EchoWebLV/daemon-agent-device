// ---------------------------------------------------------------------------
//  swap_screen.c — see swap_screen.h.
//
//  Modal layout (240×320, pixel-art / CRT-green palette):
//
//      ┌────────────────────────┐
//      │        SWAP            │
//      │                        │
//      │    5.00 USDC           │
//      │       ↓                │
//      │    0.0431 SOL          │
//      │                        │
//      │  min  0.0429 SOL       │
//      │  slip 0.5%             │
//      │  fee  0.00001 SOL      │
//      │                        │
//      │      ╭──╮              │
//      │      │ 3│  hold        │
//      │      ╰──╯              │
//      └────────────────────────┘
//
//  Hold detection: an LVGL timer at 50 Hz reads the touch indev. The
//  fill-arc reaches 100% after 3000 ms of continuous contact; release
//  before then = CANCEL_RELEASE. Any swipe gesture the LVGL indev
//  reports = CANCEL_SWIPE. 30 s with no touch at all = CANCEL_TIMEOUT.
// ---------------------------------------------------------------------------
#include "swap_screen.h"
#include "screens_common.h"

#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "lvgl.h"

#define HOLD_MS         3000
#define IDLE_TIMEOUT_MS 30000
#define POLL_PERIOD_MS  20

typedef struct {
    swap_screen_args_t args;
    SemaphoreHandle_t  done_sem;
    swap_ui_result_t  *out_result;
    lv_obj_t          *prev_screen;     // restored on close
    lv_obj_t          *scr;
    lv_obj_t          *arc;
    lv_obj_t          *arc_label;
    lv_timer_t        *poll_timer;
    uint32_t           hold_ms;          // accumulated continuous-contact ms
    uint32_t           idle_ms;          // ms since last contact
    bool               touching;
} ctx_t;

static ctx_t s_ctx;       // single in-flight modal

// Pretty-print a UI amount with up to 6 decimal places, trailing zeros stripped.
static void fmt_amount(double v, char *out, size_t cap) {
    snprintf(out, cap, "%.6f", v);
    char *dot = strchr(out, '.');
    if (!dot) return;
    char *end = out + strlen(out) - 1;
    while (end > dot && *end == '0') *end-- = '\0';
    if (end == dot) *end = '\0';
}

static void close_modal(swap_ui_result_t result) {
    if (s_ctx.poll_timer) { lv_timer_del(s_ctx.poll_timer); s_ctx.poll_timer = NULL; }
    if (s_ctx.scr)        { lv_obj_del(s_ctx.scr);          s_ctx.scr        = NULL; }
    if (s_ctx.prev_screen) {
        lv_screen_load(s_ctx.prev_screen);
        s_ctx.prev_screen = NULL;
    }
    if (s_ctx.out_result) *s_ctx.out_result = result;
    if (s_ctx.done_sem)   xSemaphoreGive(s_ctx.done_sem);
    s_ctx.done_sem = NULL;
}

static void on_swipe(lv_event_t *e) {
    (void)e;
    close_modal(SWAP_UI_CANCEL_SWIPE);
}

static void poll_tick(lv_timer_t *t) {
    (void)t;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    lv_indev_state_t state = indev ? lv_indev_get_state(indev) : LV_INDEV_STATE_RELEASED;
    bool down = (state == LV_INDEV_STATE_PRESSED);

    if (down) {
        s_ctx.hold_ms += POLL_PERIOD_MS;
        s_ctx.idle_ms  = 0;
        s_ctx.touching = true;
        int pct = (int)((s_ctx.hold_ms * 100) / HOLD_MS);
        if (pct > 100) pct = 100;
        lv_arc_set_value(s_ctx.arc, pct);
        char b[16];
        int s_left = (HOLD_MS - (int)s_ctx.hold_ms + 999) / 1000;
        if (s_left < 0) s_left = 0;
        snprintf(b, sizeof(b), "%d", s_left);
        lv_label_set_text(s_ctx.arc_label, b);
        if (s_ctx.hold_ms >= HOLD_MS) {
            close_modal(SWAP_UI_CONFIRM);
            return;
        }
    } else {
        if (s_ctx.touching && s_ctx.hold_ms > 0) {
            close_modal(SWAP_UI_CANCEL_RELEASE);
            return;
        }
        s_ctx.touching = false;
        s_ctx.idle_ms += POLL_PERIOD_MS;
        if (s_ctx.idle_ms >= IDLE_TIMEOUT_MS) {
            close_modal(SWAP_UI_CANCEL_TIMEOUT);
            return;
        }
    }
}

static void build_ui(void) {
    s_ctx.scr = lv_obj_create(NULL);
    scr_apply_bg(s_ctx.scr);

    lv_obj_t *title = lv_label_create(s_ctx.scr);
    lv_label_set_text(title, "SWAP");
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    char buf[64], amt[24];
    fmt_amount(s_ctx.args.amount_in, amt, sizeof(amt));
    snprintf(buf, sizeof(buf), "%s %s", amt, s_ctx.args.from_sym);
    lv_obj_t *l_in = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_in, buf);
    lv_obj_align(l_in, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *arrow = lv_label_create(s_ctx.scr);
    lv_label_set_text(arrow, LV_SYMBOL_DOWN);
    lv_obj_align(arrow, LV_ALIGN_TOP_MID, 0, 76);

    fmt_amount(s_ctx.args.amount_out, amt, sizeof(amt));
    snprintf(buf, sizeof(buf), "%s %s", amt, s_ctx.args.to_sym);
    lv_obj_t *l_out = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_out, buf);
    lv_obj_align(l_out, LV_ALIGN_TOP_MID, 0, 100);

    fmt_amount(s_ctx.args.min_out, amt, sizeof(amt));
    snprintf(buf, sizeof(buf), "min  %s %s", amt, s_ctx.args.to_sym);
    lv_obj_t *l_min = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_min, buf);
    lv_obj_set_style_text_color(l_min, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(l_min, LV_ALIGN_TOP_LEFT, 24, 140);

    snprintf(buf, sizeof(buf), "slip %u.%02u%%",
             s_ctx.args.slippage_bps / 100, s_ctx.args.slippage_bps % 100);
    lv_obj_t *l_slip = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_slip, buf);
    lv_obj_set_style_text_color(l_slip, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(l_slip, LV_ALIGN_TOP_LEFT, 24, 162);

    fmt_amount(s_ctx.args.fee_sol, amt, sizeof(amt));
    snprintf(buf, sizeof(buf), "fee  %s SOL", amt);
    lv_obj_t *l_fee = lv_label_create(s_ctx.scr);
    lv_label_set_text(l_fee, buf);
    lv_obj_set_style_text_color(l_fee, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(l_fee, LV_ALIGN_TOP_LEFT, 24, 184);

    s_ctx.arc = lv_arc_create(s_ctx.scr);
    lv_obj_set_size(s_ctx.arc, 80, 80);
    lv_arc_set_range(s_ctx.arc, 0, 100);
    lv_arc_set_value(s_ctx.arc, 0);
    lv_arc_set_rotation(s_ctx.arc, 270);
    lv_arc_set_bg_angles(s_ctx.arc, 0, 360);
    lv_obj_remove_style(s_ctx.arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ctx.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_ctx.arc, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ctx.arc, SCR_COLOR_ACCENT_HI, LV_PART_INDICATOR);
    lv_obj_align(s_ctx.arc, LV_ALIGN_BOTTOM_MID, 0, -36);

    s_ctx.arc_label = lv_label_create(s_ctx.scr);
    lv_label_set_text(s_ctx.arc_label, "3");
    lv_obj_center(s_ctx.arc_label);
    lv_obj_align_to(s_ctx.arc_label, s_ctx.arc, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = lv_label_create(s_ctx.scr);
    lv_label_set_text(hint, "hold to confirm");
    lv_obj_set_style_text_color(hint, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_add_event_cb(s_ctx.scr, on_swipe, LV_EVENT_GESTURE, NULL);
}

static void open_on_lvgl(void *arg) {
    (void)arg;
    if (!lvgl_port_lock(0)) return;
    s_ctx.prev_screen = lv_screen_active();
    build_ui();
    lv_screen_load(s_ctx.scr);
    s_ctx.poll_timer = lv_timer_create(poll_tick, POLL_PERIOD_MS, NULL);
    lvgl_port_unlock();
}

void swap_screen_open(const swap_screen_args_t *args,
                      SemaphoreHandle_t        done_sem,
                      swap_ui_result_t        *out_result) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.args       = *args;
    s_ctx.done_sem   = done_sem;
    s_ctx.out_result = out_result;
    if (out_result) *out_result = SWAP_UI_CANCEL_TIMEOUT;
    lv_async_call(open_on_lvgl, NULL);
}
