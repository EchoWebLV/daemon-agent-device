// ---------------------------------------------------------------------------
//  pay_confirm_screen.c — see pay_confirm_screen.h.
//
//  Modal layout (240×320, pixel-art / CRT-green palette):
//
//      ┌────────────────────────┐
//      │        Pay $0.42    [X]│
//      │                        │
//      │   <description>        │
//      │                        │
//      │                        │
//      │      ╭──╮              │
//      │      │ 3│  hold        │
//      │      ╰──╯              │
//      └────────────────────────┘
//
//  Hold detection: an LVGL timer at 50 Hz reads the touch indev. The
//  fill-arc reaches 100% after 3000 ms of continuous contact; release
//  before then = DENIED. 30 s with no touch at all = DENIED. Tapping
//  the top-right cancel button also closes with DENIED; the poll_tick
//  uses the touch point to skip hold accumulation while the user's
//  finger is on that button so the arc doesn't briefly fill on a quick tap.
//
//  PTT generation counter: if the global s_ptt_gen advances while the
//  modal is open (i.e. the user pressed PTT to interrupt), the modal
//  closes with DENIED immediately.
//
//  Swipe-gesture cancellation was intentionally omitted — the CTP panel
//  reports capacitive jitter as a gesture during a static hold, which
//  would dismiss the modal before the arc can fill (same reason as
//  swap_screen.c).
// ---------------------------------------------------------------------------
#include "pay_confirm_screen.h"
#include "screens_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "pay_confirm_screen";

// s_ptt_gen is defined in creature_screen.c and bumped on every PTT press.
// We read it inside poll_tick to cancel the modal if PTT fires mid-hold.
extern volatile uint32_t s_ptt_gen;

#define HOLD_MS              3000
#define IDLE_TIMEOUT_MS      30000
#define POLL_PERIOD_MS       20
// The touch IC reports brief RELEASED dropouts during a static hold (INT
// line retriggers between frames). Require this many continuous ms of
// RELEASED before treating it as a real lift-off.
#define RELEASE_DEBOUNCE_MS  120

// Reentrancy guard: only one modal may be open at a time.
static volatile bool s_in_use = false;

typedef struct {
    char                  title[80];     // "Pay $0.42"
    char                  desc[120];     // description text
    uint32_t              ptt_gen0;      // captured at call time
    SemaphoreHandle_t     done;
    x402_guard_decision_t decision;
    lv_obj_t             *modal;
    lv_obj_t             *arc;
    lv_obj_t             *arc_label;
    lv_obj_t             *cancel_btn;
    lv_timer_t           *poll;
    uint32_t              hold_ms;           // accumulated continuous-contact ms
    uint32_t              idle_ms;           // ms since last contact
    uint32_t              release_streak_ms; // continuous RELEASED ms (debounce)
    bool                  touching;
    bool                  closed;
} ctx_t;

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

static void close_with(ctx_t *c, x402_guard_decision_t d) {
    if (c->closed) return;
    c->closed   = true;
    c->decision = d;
    if (c->poll)  { lv_timer_del(c->poll);  c->poll  = NULL; }
    if (c->modal) { lv_obj_del(c->modal);   c->modal = NULL; }
    xSemaphoreGive(c->done);
}

// ---------------------------------------------------------------------------
// Cancel button
// ---------------------------------------------------------------------------

static void cancel_btn_cb(lv_event_t *e) {
    ctx_t *c = (ctx_t *)lv_event_get_user_data(e);
    close_with(c, X402_GUARD_CONFIRM_DENIED);
}

static bool press_on_cancel(lv_indev_t *indev, lv_obj_t *cancel_btn) {
    if (!indev || !cancel_btn) return false;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(cancel_btn, &a);
    return p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}

// ---------------------------------------------------------------------------
// Poll timer (50 Hz)
// ---------------------------------------------------------------------------

static void poll_tick(lv_timer_t *t) {
    ctx_t *c = (ctx_t *)lv_timer_get_user_data(t);
    // Already-closed guard: the cancel-button click handler can fire
    // earlier in the same LVGL iteration; bail before reading torn state.
    if (!c || c->closed) return;

    // PTT generation-counter cancel: if a new PTT press has come in while
    // we were waiting, treat it as an interrupt and deny.
    if (s_ptt_gen != c->ptt_gen0) {
        ESP_LOGD(TAG, "poll_tick: PTT gen bumped (%u→%u), closing DENIED",
                 (unsigned)c->ptt_gen0, (unsigned)s_ptt_gen);
        close_with(c, X402_GUARD_CONFIRM_DENIED);
        return;
    }

    lv_indev_t *indev = lv_indev_get_next(NULL);
    lv_indev_state_t state = indev ? lv_indev_get_state(indev) : LV_INDEV_STATE_RELEASED;
    bool down = (state == LV_INDEV_STATE_PRESSED);

    if (down) {
        // Touches that land on the cancel button must NOT fill the arc;
        // poll_tick reads the global indev so it can't tell which widget
        // the user pressed without a spatial check.
        if (press_on_cancel(indev, c->cancel_btn)) {
            c->hold_ms             = 0;
            c->idle_ms             = 0;
            c->release_streak_ms   = 0;
            c->touching            = false;
            lv_arc_set_value(c->arc, 0);
            lv_label_set_text(c->arc_label, "3");
            return;
        }
        c->hold_ms += POLL_PERIOD_MS;
        c->idle_ms  = 0;
        c->release_streak_ms = 0;
        c->touching = true;
        int pct = (int)((c->hold_ms * 100) / HOLD_MS);
        if (pct > 100) pct = 100;
        lv_arc_set_value(c->arc, pct);
        char b[16];
        int s_left = (HOLD_MS - (int)c->hold_ms + 999) / 1000;
        if (s_left < 0) s_left = 0;
        snprintf(b, sizeof(b), "%d", s_left);
        lv_label_set_text(c->arc_label, b);
        if (c->hold_ms >= HOLD_MS) {
            close_with(c, X402_GUARD_CONFIRM_OK);
            return;
        }
    } else {
        if (c->touching && c->hold_ms > 0) {
            // Debounce: a single RELEASED tick is usually a touch-IC dropout
            // mid-hold, not a real lift. Only cancel after the streak crosses
            // RELEASE_DEBOUNCE_MS of continuous releases.
            c->release_streak_ms += POLL_PERIOD_MS;
            if (c->release_streak_ms >= RELEASE_DEBOUNCE_MS) {
                close_with(c, X402_GUARD_CONFIRM_DENIED);
                return;
            }
            return;
        }
        c->touching            = false;
        c->release_streak_ms   = 0;
        c->idle_ms += POLL_PERIOD_MS;
        if (c->idle_ms >= IDLE_TIMEOUT_MS) {
            close_with(c, X402_GUARD_CONFIRM_DENIED);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Modal construction (runs on the LVGL thread via lv_async_call)
// ---------------------------------------------------------------------------

static void build_modal(void *user) {
    ctx_t *c = (ctx_t *)user;
    if (!c) return;

    if (!lvgl_port_lock(0)) {
        // Lock failure must NOT strand the caller — they're blocked on
        // done, expecting either OK or DENIED. Report DENIED immediately.
        ESP_LOGW(TAG, "lvgl_port_lock failed; aborting build_modal");
        close_with(c, X402_GUARD_CONFIRM_DENIED);
        return;
    }

    lv_obj_t *modal = lv_obj_create(NULL);
    c->modal = modal;
    scr_apply_bg(modal);

    // Title label (top-centre)
    lv_obj_t *title = lv_label_create(modal);
    lv_label_set_text(title, c->title);
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    // Top-right [X] cancel button
    c->cancel_btn = lv_button_create(modal);
    lv_obj_set_size(c->cancel_btn, 44, 32);
    lv_obj_align(c->cancel_btn, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_bg_opa(c->cancel_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(c->cancel_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(c->cancel_btn, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_set_style_radius(c->cancel_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(c->cancel_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t *cancel_lbl = lv_label_create(c->cancel_btn);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(cancel_lbl, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(c->cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, c);

    // Description sub-line
    if (c->desc[0] != '\0') {
        lv_obj_t *desc_lbl = lv_label_create(modal);
        lv_label_set_text(desc_lbl, c->desc);
        lv_obj_set_style_text_color(desc_lbl, SCR_COLOR_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(desc_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(desc_lbl, SCR_W - 32);
        lv_obj_align(desc_lbl, LV_ALIGN_TOP_MID, 0, 50);
    }

    // Hold arc (centred, bottom half)
    c->arc = lv_arc_create(modal);
    lv_obj_set_size(c->arc, 80, 80);
    lv_arc_set_range(c->arc, 0, 100);
    lv_arc_set_value(c->arc, 0);
    lv_arc_set_rotation(c->arc, 270);
    lv_arc_set_bg_angles(c->arc, 0, 360);
    lv_obj_remove_style(c->arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(c->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(c->arc, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(c->arc, SCR_COLOR_ACCENT_HI, LV_PART_INDICATOR);
    lv_obj_align(c->arc, LV_ALIGN_BOTTOM_MID, 0, -36);

    // Countdown label centred inside the arc
    c->arc_label = lv_label_create(modal);
    lv_label_set_text(c->arc_label, "3");
    lv_obj_align_to(c->arc_label, c->arc, LV_ALIGN_CENTER, 0, 0);

    // "hold to confirm" hint below the arc
    lv_obj_t *hint = lv_label_create(modal);
    lv_label_set_text(hint, "hold to confirm");
    lv_obj_set_style_text_color(hint, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(modal);

    // Start the 50 Hz poll timer. User data is the ctx pointer.
    c->poll = lv_timer_create(poll_tick, POLL_PERIOD_MS, c);

    lvgl_port_unlock();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

x402_guard_decision_t pay_confirm_screen_show_blocking(
    uint64_t actual_micros, const char *description, uint32_t ptt_gen)
{
    // Reentrancy guard: refuse a second modal while one is already showing.
    if (s_in_use) {
        ESP_LOGW(TAG, "pay_confirm_screen: already in use, returning DENIED");
        return X402_GUARD_CONFIRM_DENIED;
    }
    s_in_use = true;

    ctx_t ctx = {
        .ptt_gen0 = ptt_gen,
        .done     = xSemaphoreCreateBinary(),
        .decision = X402_GUARD_CONFIRM_DENIED,
    };

    if (!ctx.done) {
        ESP_LOGE(TAG, "pay_confirm_screen: semaphore create failed");
        s_in_use = false;
        return X402_GUARD_CONFIRM_DENIED;
    }

    snprintf(ctx.title, sizeof(ctx.title), "Pay $%.2f",
             (double)actual_micros / 1e6);
    snprintf(ctx.desc,  sizeof(ctx.desc),  "%s",
             description ? description : "");

    // Dispatch modal construction to the LVGL thread. The ctx pointer
    // remains valid for the duration of the call because the caller's stack
    // frame is kept alive by the semaphore wait below.
    lv_async_call(build_modal, &ctx);

    // Block until close_with() releases the semaphore.
    xSemaphoreTake(ctx.done, portMAX_DELAY);
    vSemaphoreDelete(ctx.done);

    s_in_use = false;
    return ctx.decision;
}
