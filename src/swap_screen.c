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
//  before then = CANCEL_RELEASE. 30 s with no touch at all =
//  CANCEL_TIMEOUT. Tapping the top-right cancel button also closes
//  with CANCEL_RELEASE; the poll_tick uses the touch point to skip
//  hold accumulation while the user's finger is on that button so the
//  arc doesn't briefly fill on a quick tap.
//
//  Swipe-gesture cancellation was removed because the CTP panel
//  reports capacitive jitter as a gesture during a static hold,
//  dismissing the modal before the arc can fill.
// ---------------------------------------------------------------------------
#include "swap_screen.h"
#include "screens_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "swap_screen";

#define HOLD_MS              3000
#define IDLE_TIMEOUT_MS      30000
#define POLL_PERIOD_MS       20
// The touch IC reports brief RELEASED dropouts during a static hold (INT
// line retriggers between frames). Require this many continuous ms of
// RELEASED before treating it as a real lift-off.
#define RELEASE_DEBOUNCE_MS  120

typedef struct {
    swap_screen_args_t args;
    SemaphoreHandle_t  done_sem;
    swap_ui_result_t  *out_result;
    lv_obj_t          *prev_screen;     // restored on close
    lv_obj_t          *scr;
    lv_obj_t          *arc;
    lv_obj_t          *arc_label;
    lv_obj_t          *cancel_btn;
    lv_timer_t        *poll_timer;
    uint32_t           hold_ms;          // accumulated continuous-contact ms
    uint32_t           idle_ms;          // ms since last contact
    uint32_t           release_streak_ms; // continuous RELEASED ms (debounce)
    bool               touching;
} ctx_t;

static ctx_t s_ctx;       // single in-flight modal

// Heap-allocated bundle that crosses the swap_screen_open → open_on_lvgl
// boundary. Required because the caller writes on its own task and the
// reader runs on the LVGL thread; passing through `lv_async_call`'s arg
// keeps everything ordered via LVGL's internal mutex.
typedef struct {
    swap_screen_args_t args;
    SemaphoreHandle_t  done_sem;
    swap_ui_result_t  *out_result;
} init_t;

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

static void on_cancel_click(lv_event_t *e) {
    (void)e;
    close_modal(SWAP_UI_CANCEL_RELEASE);
}

static bool press_on_cancel(lv_indev_t *indev) {
    if (!indev || !s_ctx.cancel_btn) return false;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(s_ctx.cancel_btn, &a);
    return p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}

static void poll_tick(lv_timer_t *t) {
    (void)t;
    // Already-closed guard: the cancel-button click handler can fire
    // earlier in the same LVGL iteration; bail before reading torn state.
    if (!s_ctx.scr) return;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    lv_indev_state_t state = indev ? lv_indev_get_state(indev) : LV_INDEV_STATE_RELEASED;
    bool down = (state == LV_INDEV_STATE_PRESSED);

    if (down) {
        // Touches that land on the cancel button must NOT fill the arc;
        // poll_tick reads the global indev so it can't tell which widget
        // the user pressed without a spatial check.
        if (press_on_cancel(indev)) {
            s_ctx.hold_ms = 0;
            s_ctx.idle_ms = 0;
            s_ctx.release_streak_ms = 0;
            s_ctx.touching = false;
            lv_arc_set_value(s_ctx.arc, 0);
            lv_label_set_text(s_ctx.arc_label, "3");
            return;
        }
        s_ctx.hold_ms += POLL_PERIOD_MS;
        s_ctx.idle_ms  = 0;
        s_ctx.release_streak_ms = 0;
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
            // Debounce: a single RELEASED tick is usually a touch-IC dropout
            // mid-hold, not a real lift. Only cancel after the streak crosses
            // RELEASE_DEBOUNCE_MS of continuous releases.
            s_ctx.release_streak_ms += POLL_PERIOD_MS;
            if (s_ctx.release_streak_ms >= RELEASE_DEBOUNCE_MS) {
                close_modal(SWAP_UI_CANCEL_RELEASE);
                return;
            }
            return;
        }
        s_ctx.touching = false;
        s_ctx.release_streak_ms = 0;
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

    s_ctx.cancel_btn = lv_button_create(s_ctx.scr);
    lv_obj_set_size(s_ctx.cancel_btn, 44, 32);
    lv_obj_align(s_ctx.cancel_btn, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_bg_opa(s_ctx.cancel_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ctx.cancel_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ctx.cancel_btn, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ctx.cancel_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_ctx.cancel_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t *cancel_lbl = lv_label_create(s_ctx.cancel_btn);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(cancel_lbl, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(s_ctx.cancel_btn, on_cancel_click, LV_EVENT_CLICKED, NULL);

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
}

static void open_on_lvgl(void *arg) {
    init_t *init = (init_t *)arg;
    if (!init) return;
    if (!lvgl_port_lock(0)) {
        // Lock failure must NOT strand the caller — they're blocked on
        // done_sem expecting either CONFIRM or a CANCEL_*. Report timeout
        // immediately so the caller can retry or surface the failure.
        ESP_LOGW(TAG, "lvgl_port_lock failed; aborting open");
        if (init->out_result) *init->out_result = SWAP_UI_CANCEL_TIMEOUT;
        if (init->done_sem)   xSemaphoreGive(init->done_sem);
        free(init);
        return;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.args       = init->args;
    s_ctx.done_sem   = init->done_sem;
    s_ctx.out_result = init->out_result;
    s_ctx.prev_screen = lv_screen_active();
    build_ui();
    lv_screen_load(s_ctx.scr);
    s_ctx.poll_timer = lv_timer_create(poll_tick, POLL_PERIOD_MS, NULL);
    lvgl_port_unlock();
    free(init);
}

void swap_screen_open(const swap_screen_args_t *args,
                      SemaphoreHandle_t        done_sem,
                      swap_ui_result_t        *out_result) {
    if (out_result) *out_result = SWAP_UI_CANCEL_TIMEOUT;
    init_t *init = malloc(sizeof(*init));
    if (!init) {
        ESP_LOGW(TAG, "init alloc failed");
        if (done_sem) xSemaphoreGive(done_sem);
        return;
    }
    init->args       = *args;
    init->done_sem   = done_sem;
    init->out_result = out_result;
    lv_async_call(open_on_lvgl, init);
}
