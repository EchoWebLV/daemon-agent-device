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

// Panel is 240×320 (portrait). Keep layout constants here so each screen
// module doesn't redefine them.
#define SCR_W            240
#define SCR_H            320
#define STATUS_BAR_H      26

// Palette. Uses the Daemon-blue family from the Arduino build, re-tuned
// for LVGL's 24-bit color space. The TFT_eSPI RGB565 constants don't map
// directly, so we just pick visually-matching full-precision colors.
#define SCR_COLOR_BG         lv_color_hex(0x001428)   // dark navy
#define SCR_COLOR_PANEL      lv_color_hex(0x0B1E3A)   // card background
#define SCR_COLOR_ACCENT     lv_color_hex(0x4DA6FF)   // Daemon blue
#define SCR_COLOR_ACCENT_HI  lv_color_hex(0x7AD8FF)   // cyan rim
#define SCR_COLOR_TEXT       lv_color_hex(0xE6E6FA)   // near white
#define SCR_COLOR_DIM        lv_color_hex(0x6F8AB8)   // muted blue
#define SCR_COLOR_DIVIDER    lv_color_hex(0x1E3A5F)   // separator line
#define SCR_COLOR_GOOD       lv_color_hex(0x3EE07A)   // "connected" green
#define SCR_COLOR_WARN       lv_color_hex(0xFFB84D)   // warning amber

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
