// ---------------------------------------------------------------------------
//  Creature home screen — Daemon's face.
//
//  Layout (240x320):
//
//    +--------------------------------------------------+
//    | status                       SOL $198.42         |
//    |                              USDC 12.34          |
//    +--------------------------------------------------+
//    |                                                  |
//    |            \         /                           |
//    |             \       /      <- ears (lv_line)     |
//    |        +---+         +---+                       |
//    |        |                  |                      |
//    |        |   ●           ●  | <- eyes (pulse)      |
//    |        |                  |                      |
//    |        |       ===        | <- mouth (grows      |
//    |        +------------------+     when talking)    |
//    |                                                  |
//    |       "hello, friend"   <- subtitle              |
//    +--------------------------------------------------+
//
//  Everything here is drawn with plain LVGL widgets and lv_anim_t, so the
//  look-and-feel carries across themes (LV_COLOR_DEPTH=16 on device,
//  32 in simulator). No pixel blitting, no fonts beyond the default.
//
//  Thread-safety note: all of the setters below lock the lvgl_port mutex
//  and then mutate widgets. The tick function runs on the main loop and
//  does the same. LVGL's own timer task runs under the same mutex, so
//  we're safe from animation-vs-mutation races.
// ---------------------------------------------------------------------------
#include "creature_screen.h"
#include "screens_common.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "creature_screen";

// --- widget handles --------------------------------------------------------
static lv_obj_t *s_scr          = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_price_label  = NULL;
static lv_obj_t *s_usdc_label   = NULL;
static lv_obj_t *s_body         = NULL;
static lv_obj_t *s_eye_l        = NULL;
static lv_obj_t *s_eye_r        = NULL;
static lv_obj_t *s_mouth        = NULL;
static lv_obj_t *s_ear_l        = NULL;
static lv_obj_t *s_ear_r        = NULL;
static lv_obj_t *s_subtitle     = NULL;

// Mutable line-point buffers for the ears. lv_line_set_points just stores
// the pointer, so these must outlive the line widget — module-level is
// the simplest correct home.
static lv_point_precise_t s_ear_l_pts[3];
static lv_point_precise_t s_ear_r_pts[3];

// --- mood + talking state --------------------------------------------------
static creature_mood_t s_mood    = CREATURE_MOOD_IDLE;
static bool            s_talking = false;

// Mouth animation — driven by an internal lv_timer on the LVGL task so
// the main loop doesn't have to pump it at a particular cadence. A manual
// tick instead of lv_anim_t lets us stop the mouth mid-open the instant
// talking flips to false.
static lv_timer_t *s_mouth_timer = NULL;
static uint32_t    s_mouth_phase = 0;  // ms since talking started

// --- helpers ---------------------------------------------------------------

static lv_color_t mood_color(creature_mood_t m) {
    switch (m) {
        case CREATURE_MOOD_IDLE:   return SCR_COLOR_ACCENT;
        case CREATURE_MOOD_LISTEN: return SCR_COLOR_ACCENT_HI;
        case CREATURE_MOOD_THINK:  return SCR_COLOR_DIM;
        case CREATURE_MOOD_TALK:   return SCR_COLOR_ACCENT_HI;
        case CREATURE_MOOD_HAPPY:  return SCR_COLOR_GOOD;
        case CREATURE_MOOD_ANGRY:  return SCR_COLOR_WARN;
    }
    return SCR_COLOR_ACCENT;
}

// Eye pulse animation: opacity 80 -> 255 -> 80, 2000 ms total, infinite.
// One instance per eye so they share the same curve but each gets its own
// timer (matches up to a single frame — good enough for a face).
static void anim_eye_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

static void install_eye_pulse(lv_obj_t *eye) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, eye);
    lv_anim_set_values(&a, 120, 255);
    lv_anim_set_time(&a, 1100);
    lv_anim_set_playback_time(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_eye_opa_cb);
    lv_anim_start(&a);
}

// Mouth "chatter": four-step height cycle that reads as talking. Separate
// from the lv_timer callback so creature_screen_tick() can advance the
// mouth by one frame on demand without touching the timer state.
static void mouth_step(void) {
    if (!s_mouth) return;
    static const int16_t h_cycle[] = {4, 14, 22, 10};
    s_mouth_phase += 80;
    uint32_t idx = (s_mouth_phase / 120) %
                   (sizeof(h_cycle) / sizeof(h_cycle[0]));
    int16_t h = h_cycle[idx];
    lv_obj_set_size(s_mouth, 60, h);
    // Re-anchor center so height changes grow around the middle of the
    // mouth, not the top edge.
    lv_obj_align(s_mouth, LV_ALIGN_CENTER, 0, 36);
}

// lv_timer callback. Runs on the LVGL task with the port mutex held, so
// no explicit lock here.
static void mouth_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_talking) return;   // defensive — paused timer shouldn't fire
    mouth_step();
}

// Build a rounded-rect widget styled as "creature chrome" — panel fill,
// accent-hi border, radius baked in. Used for the body and the mouth rail.
static lv_obj_t *make_panel(lv_obj_t *parent, int w, int h, int radius,
                            lv_color_t border_color, int border_width) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, border_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, border_width, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// --- public API ------------------------------------------------------------

bool creature_screen_init(void) {
    if (s_scr) return true;   // already built

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        return false;
    }

    // --- root screen ------------------------------------------------------
    s_scr = lv_obj_create(NULL);
    scr_apply_bg(s_scr);

    // --- status bar: we build it by hand here so we can reach both the
    //     right "price" label and a below-it "usdc" label. scr_make_status_bar
    //     only exposes a single right label, which isn't enough. -----------
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, SCR_W, STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 4, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(bar);
    lv_label_set_text(s_status_label, "");
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

    // --- creature container (ears live above it, so we carve out space
    //     for them between the status bar and the body) -------------------
    //     ears: y ~ 40..90
    //     body: y ~ 90..250 (160 tall, 180 wide)
    //     subtitle: y ~ 260..315

    // Body rounded rect: 180×160, centered horizontally, under ears.
    s_body = make_panel(s_scr, 180, 160, 60,
                        SCR_COLOR_ACCENT_HI, 3);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 88);

    // Ears: two 3-point polylines forming triangles above the body.
    // Left ear roots at (60, 90) -> peak (70, 40) -> inner (90, 90).
    // Right ear mirrors. All coords are in screen space (parent = scr).
    s_ear_l_pts[0].x =  60; s_ear_l_pts[0].y =  95;
    s_ear_l_pts[1].x =  74; s_ear_l_pts[1].y =  40;
    s_ear_l_pts[2].x =  98; s_ear_l_pts[2].y =  95;

    s_ear_r_pts[0].x = 142; s_ear_r_pts[0].y =  95;
    s_ear_r_pts[1].x = 166; s_ear_r_pts[1].y =  40;
    s_ear_r_pts[2].x = 180; s_ear_r_pts[2].y =  95;

    s_ear_l = lv_line_create(s_scr);
    lv_line_set_points(s_ear_l, s_ear_l_pts, 3);
    lv_obj_set_style_line_color(s_ear_l, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_ear_l, 4, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s_ear_l, true, LV_PART_MAIN);

    s_ear_r = lv_line_create(s_scr);
    lv_line_set_points(s_ear_r, s_ear_r_pts, 3);
    lv_obj_set_style_line_color(s_ear_r, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_ear_r, 4, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s_ear_r, true, LV_PART_MAIN);

    // Eyes: circles as children of the body so they stay aligned if the
    // body moves later. 22px diameter, radius=50% for a perfect circle.
    s_eye_l = lv_obj_create(s_body);
    lv_obj_set_size(s_eye_l, 22, 22);
    lv_obj_set_style_radius(s_eye_l, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_eye_l, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_eye_l, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_eye_l, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_eye_l, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_eye_l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_eye_l, LV_ALIGN_CENTER, -38, -18);

    s_eye_r = lv_obj_create(s_body);
    lv_obj_set_size(s_eye_r, 22, 22);
    lv_obj_set_style_radius(s_eye_r, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_eye_r, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_eye_r, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_eye_r, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_eye_r, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_eye_r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_eye_r, LV_ALIGN_CENTER, 38, -18);

    install_eye_pulse(s_eye_l);
    install_eye_pulse(s_eye_r);

    // Mouth timer fires at 80 ms while talking; paused otherwise. Running
    // on LVGL's own timer loop means the mouth animates smoothly regardless
    // of main-loop cadence, and we don't need the port mutex inside the
    // callback (LVGL already holds it).
    s_mouth_timer = lv_timer_create(mouth_timer_cb, 80, NULL);
    lv_timer_pause(s_mouth_timer);

    // Mouth: short horizontal bar at rest; creature_screen_tick() resizes
    // it while talking. Child of body, aligned lower third.
    s_mouth = lv_obj_create(s_body);
    lv_obj_set_size(s_mouth, 60, 4);
    lv_obj_set_style_radius(s_mouth, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mouth, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_mouth, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_mouth, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_mouth, LV_ALIGN_CENTER, 0, 36);

    // --- subtitle ---------------------------------------------------------
    s_subtitle = lv_label_create(s_scr);
    lv_label_set_text(s_subtitle, "");
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_subtitle, SCR_W - 24);
    lv_obj_set_style_text_align(s_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_subtitle, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_subtitle, LV_ALIGN_BOTTOM_MID, 0, -8);

    // Apply the initial mood tint.
    creature_mood_t m = s_mood;
    s_mood = (creature_mood_t)-1;  // force set path
    lvgl_port_unlock();
    creature_screen_set_mood(m);

    ESP_LOGI(TAG, "creature screen built");
    return true;
}

lv_obj_t *creature_screen(void) { return s_scr; }

void creature_screen_set_status(const char *s) {
    if (!s_status_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_label, s ? s : "");
    lvgl_port_unlock();
}

void creature_screen_set_price(const char *s) {
    if (!s_price_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_price_label, s ? s : "");
    lvgl_port_unlock();
}

void creature_screen_set_usdc(const char *s) {
    if (!s_usdc_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_usdc_label, s ? s : "");
    lvgl_port_unlock();
}

void creature_screen_set_subtitle(const char *text) {
    if (!s_subtitle) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_subtitle, text ? text : "");
    lvgl_port_unlock();
}

void creature_screen_set_mood(creature_mood_t m) {
    if (!s_scr) return;
    if (m == s_mood) return;
    s_mood = m;

    lv_color_t border = mood_color(m);

    if (!lvgl_port_lock(0)) return;
    // Body border + eyes + mouth + ears all follow mood colour so the face
    // reads at a glance.
    lv_obj_set_style_border_color(s_body, border, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_eye_l, border, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_eye_r, border, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mouth, border, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_ear_l, border, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_ear_r, border, LV_PART_MAIN);
    lvgl_port_unlock();
}

void creature_screen_set_talking(bool on) {
    if (s_talking == on) return;
    s_talking = on;

    if (!lvgl_port_lock(0)) return;
    if (on) {
        s_mouth_phase = 0;
        if (s_mouth_timer) lv_timer_resume(s_mouth_timer);
    } else {
        if (s_mouth_timer) lv_timer_pause(s_mouth_timer);
        s_mouth_phase = 0;
        if (s_mouth) {
            lv_obj_set_size(s_mouth, 60, 4);
            lv_obj_align(s_mouth, LV_ALIGN_CENTER, 0, 36);
        }
    }
    lvgl_port_unlock();
}

// Retained for API completeness + future callers (e.g. an integration
// layer that wants to force a redraw between timer ticks). The mouth's
// regular cadence is now driven by the internal lv_timer installed in
// creature_screen_init(), so this is a no-op when talking is off.
void creature_screen_tick(void) {
    if (!s_talking || !s_mouth) return;
    if (!lvgl_port_lock(0)) return;
    mouth_step();
    lvgl_port_unlock();
}
