// ---------------------------------------------------------------------------
//  Creature home screen — Daemon's face, rendered as pixel art.
//
//  Retro CRT-green pixels on pure black. Each face feature is built from
//  chunky 10×10 blocks so the whole thing reads as "analogue / Tamagotchi"
//  rather than "smooth vector". No images or custom fonts — every pixel
//  is its own lv_obj rectangle with radius=0, styled from the palette.
//
//  Layout (240×320):
//
//    +--------------------------------------------------+
//    | USDC 12.34                     SOL $198.42       |
//    +--------------------------------------------------+
//    |  ▓             ▓     <- eyebrow tips             |
//    |  ▓             ▓                                 |
//    |   ▓           ▓      <- diagonal zigzag          |
//    |   ▓           ▓                                  |
//    |    ▓         ▓       <- bases above eyes         |
//    |                                                  |
//    |    ███       ███     <- eyes (30×30, pulse)      |
//    |    ███       ███                                 |
//    |                                                  |
//    | ▓                 ▓  <- smile tips rise wide     |
//    |  ▓               ▓                               |
//    |   ▓▓▓▓▓▓▓▓▓▓▓▓▓▓     <- 10-pixel base arc        |
//    |                                                  |
//    |       "hello, friend"   <- subtitle              |
//    +--------------------------------------------------+
//
//  Thread-safety note: all setters lock the lvgl_port mutex before touching
//  widgets. LVGL's own timer task runs under the same mutex.
// ---------------------------------------------------------------------------
#include "creature_screen.h"
#include "screens_common.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "creature_screen";

// Pixel size — base unit for every face feature.
#define PX              10
#define EYE_W           30
#define EYE_H           30
#define MOUTH_W         60
#define EYE_L_X         75
#define EYE_R_X        (SCR_W - EYE_L_X - EYE_W)
#define EYE_Y          115

// Shift every face block down by this much from the coordinates listed in
// the k_* tables below. Tweak in one place to raise/lower the face without
// touching each feature's Y column.
#define FACE_Y_OFFSET   20
#define SMILE_MID_Y    (205 + FACE_Y_OFFSET)    // where the talking bar centers

// --- widget handles --------------------------------------------------------
static lv_obj_t *s_scr          = NULL;
// Top-left slot shows USDC; SOL price on the right.
static lv_obj_t *s_price_label  = NULL;
static lv_obj_t *s_usdc_label   = NULL;

// Face layers (all children of s_scr, painted on the black root).
static lv_obj_t *s_eye_l        = NULL;
static lv_obj_t *s_eye_r        = NULL;
static lv_obj_t *s_mouth        = NULL;   // animated bar used while talking
static lv_obj_t *s_ant_l[5]     = {0};    // left eyebrow (5 pixels)
static lv_obj_t *s_ant_r[5]     = {0};    // right eyebrow (5 pixels)
static lv_obj_t *s_smile[14]    = {0};    // 14-pixel smile shown while idle

static lv_obj_t *s_subtitle     = NULL;

// Pixel coordinates for static face features. Kept at module scope (not
// lookup tables at init) so the geometry is easy to tweak in one place.
// Eyebrows — 5-pixel diagonal-zigzag strips slanting from the outer top
// corner down toward each eye. Two stacked pixels at the tip, one step
// inner, two stacked below, and one more step inner at the base read as
// a clear diagonal (matches the reference sketch).
static const int16_t k_ant_l[5][2] = {
    { 60, 40}, { 60, 50},     // tip, stacked outer
    { 70, 60},                 // step inner
    { 70, 70},                 // stacked
    { 80, 80},                 // base — above left eye
};
static const int16_t k_ant_r[5][2] = {
    {170, 40}, {170, 50},
    {160, 60},
    {160, 70},
    {150, 80},
};
// Smile — 14-pixel arc, three rows. Outer tips rise two rows above a
// ten-pixel base, giving the face a much wider grin than before.
static const int16_t k_smile[14][2] = {
    // row 0 — outermost rising tips
    { 50, 185}, {180, 185},
    // row 1 — inner shoulders one column in
    { 60, 195}, {170, 195},
    // row 2 — base of smile arc (10 pixels wide)
    { 70, 205}, { 80, 205}, { 90, 205}, {100, 205},
    {110, 205}, {120, 205}, {130, 205}, {140, 205},
    {150, 205}, {160, 205},
};

// --- mood + talking state --------------------------------------------------
static creature_mood_t s_mood    = CREATURE_MOOD_IDLE;
static bool            s_talking = false;

// Mouth animation driven by an LVGL timer so cadence is independent of the
// main loop. Manual stepping (not lv_anim_t) lets us halt the mouth the
// instant talking flips off.
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

// Strip every default decoration from a fresh lv_obj and style it as a
// hard-edged, fully-opaque rectangle. Foundation of every face pixel.
static lv_obj_t *make_block(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// Eye pulse: opacity 120 → 255 → 120 at 1100 ms each way. Runs forever on
// LVGL's animation timer so each eye gets its own curve instance.
static void anim_eye_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

static void install_eye_pulse(lv_obj_t *eye) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, eye);
    lv_anim_set_values(&a, 140, 255);
    lv_anim_set_time(&a, 1100);
    lv_anim_set_playback_time(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_eye_opa_cb);
    lv_anim_start(&a);
}

// Four-step mouth height cycle read as talking chatter. Called from the
// LVGL timer while s_talking is true.
static void mouth_step(void) {
    if (!s_mouth) return;
    static const int16_t h_cycle[] = {4, 14, 22, 10};
    s_mouth_phase += 80;
    uint32_t idx = (s_mouth_phase / 120) %
                   (sizeof(h_cycle) / sizeof(h_cycle[0]));
    int16_t h = h_cycle[idx];
    lv_obj_set_size(s_mouth, MOUTH_W, h);
    // Re-anchor so the bar grows around its midline rather than the top
    // edge, keeping the mouth centered in the smile slot.
    lv_obj_set_pos(s_mouth, (SCR_W - MOUTH_W) / 2, SMILE_MID_Y - h / 2);
}

static void mouth_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_talking) return;
    mouth_step();
}

// Show/hide the static smile block. Called from set_talking().
static void set_smile_visible(bool visible) {
    for (size_t i = 0; i < sizeof(s_smile) / sizeof(s_smile[0]); i++) {
        if (!s_smile[i]) continue;
        if (visible) lv_obj_remove_flag(s_smile[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag   (s_smile[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// --- public API ------------------------------------------------------------

bool creature_screen_init(void) {
    if (s_scr) return true;

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        return false;
    }

    // --- root screen ------------------------------------------------------
    s_scr = lv_obj_create(NULL);
    scr_apply_bg(s_scr);

    // --- status bar: USDC (left), SOL price (right) -----------------------
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, SCR_W, STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 4, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_usdc_label = lv_label_create(bar);
    lv_label_set_text(s_usdc_label, "");
    lv_obj_set_style_text_color(s_usdc_label, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(s_usdc_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_price_label = lv_label_create(bar);
    lv_label_set_text(s_price_label, "");
    lv_obj_set_style_text_color(s_price_label, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(s_price_label, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_color_t face = SCR_COLOR_ACCENT;

    // --- eyebrows: two 5-pixel diagonal zigzags --------------------------
    for (size_t i = 0; i < sizeof(s_ant_l) / sizeof(s_ant_l[0]); i++) {
        s_ant_l[i] = make_block(s_scr, k_ant_l[i][0],
                                k_ant_l[i][1] + FACE_Y_OFFSET, PX, PX, face);
        s_ant_r[i] = make_block(s_scr, k_ant_r[i][0],
                                k_ant_r[i][1] + FACE_Y_OFFSET, PX, PX, face);
    }

    // --- eyes: small 30×30 blocks (no rounded corners — pixel aesthetic) -
    s_eye_l = make_block(s_scr, EYE_L_X, EYE_Y + FACE_Y_OFFSET,
                         EYE_W, EYE_H, face);
    s_eye_r = make_block(s_scr, EYE_R_X, EYE_Y + FACE_Y_OFFSET,
                         EYE_W, EYE_H, face);
    install_eye_pulse(s_eye_l);
    install_eye_pulse(s_eye_r);

    // --- smile: 14 pixel blocks forming a wide curved-upward arc ---------
    for (size_t i = 0; i < sizeof(s_smile) / sizeof(s_smile[0]); i++) {
        s_smile[i] = make_block(s_scr, k_smile[i][0],
                                k_smile[i][1] + FACE_Y_OFFSET, PX, PX, face);
    }

    // --- talking mouth: single rectangle, hidden until talking starts -----
    s_mouth = make_block(s_scr, (SCR_W - MOUTH_W) / 2, SMILE_MID_Y - 2,
                         MOUTH_W, 4, face);
    lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);

    s_mouth_timer = lv_timer_create(mouth_timer_cb, 80, NULL);
    lv_timer_pause(s_mouth_timer);

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
    s_mood = (creature_mood_t)-1;   // force set path on first call
    lvgl_port_unlock();
    creature_screen_set_mood(m);

    ESP_LOGI(TAG, "creature screen built (pixel face)");
    return true;
}

lv_obj_t *creature_screen(void) { return s_scr; }

void creature_screen_set_status(const char *s) {
    // Intentional no-op. USDC lives in the top-left slot now; IP moved
    // to the serial log. Kept so ui_set_status(ip) remains harmless.
    (void)s;
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

    lv_color_t c = mood_color(m);

    if (!lvgl_port_lock(0)) return;
    // Repaint every face block so the whole creature reads the same mood.
    for (size_t i = 0; i < sizeof(s_ant_l) / sizeof(s_ant_l[0]); i++) {
        if (s_ant_l[i]) lv_obj_set_style_bg_color(s_ant_l[i], c, LV_PART_MAIN);
        if (s_ant_r[i]) lv_obj_set_style_bg_color(s_ant_r[i], c, LV_PART_MAIN);
    }
    if (s_eye_l) lv_obj_set_style_bg_color(s_eye_l, c, LV_PART_MAIN);
    if (s_eye_r) lv_obj_set_style_bg_color(s_eye_r, c, LV_PART_MAIN);
    for (size_t i = 0; i < sizeof(s_smile) / sizeof(s_smile[0]); i++) {
        if (s_smile[i]) lv_obj_set_style_bg_color(s_smile[i], c, LV_PART_MAIN);
    }
    if (s_mouth) lv_obj_set_style_bg_color(s_mouth, c, LV_PART_MAIN);
    lvgl_port_unlock();
}

void creature_screen_set_talking(bool on) {
    if (s_talking == on) return;
    s_talking = on;

    if (!lvgl_port_lock(0)) return;
    if (on) {
        // Hide the smile and reveal the animated bar. Timer drives height.
        set_smile_visible(false);
        lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
        s_mouth_phase = 0;
        if (s_mouth_timer) lv_timer_resume(s_mouth_timer);
    } else {
        if (s_mouth_timer) lv_timer_pause(s_mouth_timer);
        s_mouth_phase = 0;
        // Reset mouth bar and hide it so the smile is cleanly visible again.
        if (s_mouth) {
            lv_obj_set_size(s_mouth, MOUTH_W, 4);
            lv_obj_set_pos(s_mouth, (SCR_W - MOUTH_W) / 2, SMILE_MID_Y - 2);
            lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
        }
        set_smile_visible(true);
    }
    lvgl_port_unlock();
}

// Retained so an integration layer can force a mouth frame between timer
// ticks. The regular cadence is already driven by the lv_timer installed
// in creature_screen_init(), so this is a no-op when talking is off.
void creature_screen_tick(void) {
    if (!s_talking || !s_mouth) return;
    if (!lvgl_port_lock(0)) return;
    mouth_step();
    lvgl_port_unlock();
}
