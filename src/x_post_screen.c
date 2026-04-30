// ---------------------------------------------------------------------------
//  Approval modal for X posts. See x_post_screen.h.
// ---------------------------------------------------------------------------
#include "x_post_screen.h"
#include "screens_common.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"

static const char *TAG = "x_post_screen";

#define HOLD_TO_CONFIRM_MS  1500
#define MODAL_TIMEOUT_MS    30000
#define POLL_PERIOD_MS      50
#define CANCEL_SWIPE_PX     60

typedef struct {
    x_post_screen_args_t  args;
    SemaphoreHandle_t     done_sem;
    x_post_ui_result_t   *out_result;
    lv_obj_t             *scr;
    lv_obj_t             *prev_screen;
    lv_obj_t             *progress;
    lv_timer_t           *poll_timer;
    int64_t               opened_at_us;
    int64_t               press_start_us;
    int                   touch_start_x;
    bool                  pressed;
} ctx_t;

static ctx_t s_ctx;

typedef struct {
    x_post_screen_args_t args;
    SemaphoreHandle_t    done_sem;
    x_post_ui_result_t  *out_result;
} init_t;

static void close_modal(x_post_ui_result_t result) {
    if (s_ctx.poll_timer) { lv_timer_del(s_ctx.poll_timer); s_ctx.poll_timer = NULL; }
    if (s_ctx.scr)        { lv_obj_del(s_ctx.scr);          s_ctx.scr        = NULL; }
    if (s_ctx.prev_screen) {
        lv_screen_load(s_ctx.prev_screen);
        s_ctx.prev_screen = NULL;
    }
    if (s_ctx.out_result) *s_ctx.out_result = result;
    if (s_ctx.done_sem)   xSemaphoreGive(s_ctx.done_sem);
    s_ctx.done_sem   = NULL;
    s_ctx.out_result = NULL;
}

static void poll_tick(lv_timer_t *t) {
    (void)t;
    if (!s_ctx.scr) return;
    int64_t now = esp_timer_get_time();

    if (now - s_ctx.opened_at_us > (int64_t)MODAL_TIMEOUT_MS * 1000) {
        close_modal(X_POST_UI_CANCEL_TIMEOUT);
        return;
    }

    lv_indev_t *indev = lv_indev_get_next(NULL);
    lv_indev_state_t state = indev ? lv_indev_get_state(indev) : LV_INDEV_STATE_RELEASED;
    bool down = (state == LV_INDEV_STATE_PRESSED);
    lv_point_t p = {0};
    if (down && indev) lv_indev_get_point(indev, &p);

    if (down && !s_ctx.pressed) {
        s_ctx.pressed = true;
        s_ctx.press_start_us = now;
        s_ctx.touch_start_x = p.x;
    } else if (down && s_ctx.pressed) {
        if (p.x - s_ctx.touch_start_x < -CANCEL_SWIPE_PX) {
            close_modal(X_POST_UI_CANCEL_SWIPE);
            return;
        }
        int64_t held = now - s_ctx.press_start_us;
        int pct = (int)(held * 100 / ((int64_t)HOLD_TO_CONFIRM_MS * 1000));
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (s_ctx.progress) lv_bar_set_value(s_ctx.progress, pct, LV_ANIM_OFF);
        if (held >= (int64_t)HOLD_TO_CONFIRM_MS * 1000) {
            close_modal(X_POST_UI_CONFIRM);
            return;
        }
    } else if (!down && s_ctx.pressed) {
        close_modal(X_POST_UI_CANCEL_RELEASE);
        return;
    }
}

static void build_ui(void) {
    s_ctx.scr = lv_obj_create(NULL);
    scr_apply_bg(s_ctx.scr);
    lv_obj_set_style_pad_all(s_ctx.scr, 12, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_ctx.scr);
    lv_label_set_text(title, "POST TO X?");
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    if (s_ctx.args.handle[0]) {
        lv_obj_t *who = lv_label_create(s_ctx.scr);
        char buf[40];
        snprintf(buf, sizeof(buf), "as @%s", s_ctx.args.handle);
        lv_label_set_text(who, buf);
        lv_obj_set_style_text_color(who, SCR_COLOR_DIM, LV_PART_MAIN);
        lv_obj_align(who, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    lv_obj_t *body = lv_label_create(s_ctx.scr);
    lv_label_set_text(body, s_ctx.args.text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, SCR_W - 24);
    lv_obj_set_style_text_color(body, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 28);

    lv_obj_t *hint = lv_label_create(s_ctx.scr);
    lv_label_set_text(hint, "hold to confirm  |  swipe < to cancel");
    lv_obj_set_style_text_color(hint, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -28);

    s_ctx.progress = lv_bar_create(s_ctx.scr);
    lv_obj_set_size(s_ctx.progress, SCR_W - 48, 8);
    lv_bar_set_range(s_ctx.progress, 0, 100);
    lv_bar_set_value(s_ctx.progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ctx.progress, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ctx.progress, SCR_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_align(s_ctx.progress, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static void open_on_lvgl(void *arg) {
    init_t *init = (init_t *)arg;
    if (!init) return;
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(TAG, "lvgl_port_lock failed; aborting open");
        if (init->out_result) *init->out_result = X_POST_UI_CANCEL_TIMEOUT;
        if (init->done_sem)   xSemaphoreGive(init->done_sem);
        free(init);
        return;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.args         = init->args;
    s_ctx.done_sem     = init->done_sem;
    s_ctx.out_result   = init->out_result;
    s_ctx.prev_screen  = lv_screen_active();
    s_ctx.opened_at_us = esp_timer_get_time();
    build_ui();
    lv_screen_load(s_ctx.scr);
    s_ctx.poll_timer = lv_timer_create(poll_tick, POLL_PERIOD_MS, NULL);
    lvgl_port_unlock();
    free(init);
}

void x_post_screen_open(const x_post_screen_args_t *args,
                        SemaphoreHandle_t          done_sem,
                        x_post_ui_result_t        *out_result) {
    if (out_result) *out_result = X_POST_UI_CANCEL_TIMEOUT;
    init_t *init = malloc(sizeof(*init));
    if (!init) {
        if (done_sem) xSemaphoreGive(done_sem);
        return;
    }
    init->args       = *args;
    init->done_sem   = done_sem;
    init->out_result = out_result;
    lv_async_call(open_on_lvgl, init);
}
