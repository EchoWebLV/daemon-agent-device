// ---------------------------------------------------------------------------
//  Shared palette + style helpers for the four Daemon screens.
//
//  Keeping the colors + shared setup in one place means a theme tweak
//  (e.g. swapping the accent blue) is a two-line edit, not a 1000-line
//  sweep. Every screen module pulls the same fonts + background so screen
//  swaps are visually coherent.
// ---------------------------------------------------------------------------
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Panel is 320×240 (landscape) on the ESP32-S3-BOX-3. Keep layout
// constants here so each screen module doesn't redefine them.
#define SCR_W            320
#define SCR_H            240
#define STATUS_BAR_H      26

// Palette — pixel-art CRT green on pure black by default; flipped to
// black-on-white when the user toggles the light theme. Each colour is
// a runtime variable (not a #define literal) so theme switching works
// without touching every call site. The macros below preserve the old
// "named constant" syntax — call sites read `SCR_COLOR_BG` and never
// see the indirection. ui.c owns the storage and the theme applier.
extern lv_color_t scr_palette_bg;
extern lv_color_t scr_palette_panel;
extern lv_color_t scr_palette_accent;
extern lv_color_t scr_palette_accent_hi;
extern lv_color_t scr_palette_text;
extern lv_color_t scr_palette_dim;
extern lv_color_t scr_palette_divider;
extern lv_color_t scr_palette_good;
extern lv_color_t scr_palette_warn;

#define SCR_COLOR_BG         (scr_palette_bg)         // bg fill
#define SCR_COLOR_PANEL      (scr_palette_panel)      // card / inset surface
#define SCR_COLOR_ACCENT     (scr_palette_accent)     // primary fg (text, lines)
#define SCR_COLOR_ACCENT_HI  (scr_palette_accent_hi)  // brighter fg (highlights)
#define SCR_COLOR_TEXT       (scr_palette_text)       // body text
#define SCR_COLOR_DIM        (scr_palette_dim)        // placeholders / muted
#define SCR_COLOR_DIVIDER    (scr_palette_divider)    // hairline rules
#define SCR_COLOR_GOOD       (scr_palette_good)       // success / accent_hi alias
#define SCR_COLOR_WARN       (scr_palette_warn)       // warning amber (theme-independent)

// Apply the theme to the runtime palette. Call once at boot (in ui_init,
// before any screen is built) and after toggling devcfg_set_theme().
// Switching at runtime requires a restart for existing widgets to
// repaint — the palette only governs colours read at widget-construction
// time. theme: 0 = dark (default), 1 = light.
void scr_palette_apply(int theme);

// Dark fill applied to any screen root. Call right after lv_obj_create().
static inline void scr_apply_bg(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa  (scr, LV_OPA_COVER,   LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

// Top status bar with a title on the left and an optional ticker on the
// right. Returns the bar's parent obj; `out_right_label` receives the
// ticker label so the caller can lv_label_set_text() it.
static inline lv_obj_t *scr_make_status_bar(lv_obj_t *parent,
                                            const char *title,
                                            lv_obj_t **out_right_label) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCR_W, STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, SCR_COLOR_BG,  LV_PART_MAIN);
    lv_obj_set_style_bg_opa  (bar, LV_OPA_COVER,  LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all (bar, 4, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = lv_label_create(bar);
    lv_label_set_text(left, title ? title : "");
    lv_obj_set_style_text_color(left, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *right = lv_label_create(bar);
    lv_label_set_text(right, "");
    lv_obj_set_style_text_color(right, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);

    if (out_right_label) *out_right_label = right;
    return bar;
}

#ifdef __cplusplus
}
#endif
