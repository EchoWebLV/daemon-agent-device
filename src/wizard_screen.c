// ---------------------------------------------------------------------------
//  Wizard splash. See wizard_screen.h.
// ---------------------------------------------------------------------------
#include "wizard_screen.h"

#include <string.h>

#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "screens_common.h"

void wizard_screen_show(const char *ssid) {
    if (!lvgl_port_lock(0)) return;

    lv_obj_t *scr = lv_obj_create(NULL);
    scr_apply_bg(scr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "FIRST-RUN SETUP");
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *step1 = lv_label_create(scr);
    lv_label_set_text(step1, "1. Join Wi-Fi:");
    lv_obj_set_style_text_color(step1, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(step1, LV_ALIGN_TOP_LEFT, 16, 76);

    lv_obj_t *ssid_lbl = lv_label_create(scr);
    lv_label_set_text(ssid_lbl, ssid ? ssid : "");
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ssid_lbl, SCR_W - 48);
    lv_obj_set_style_text_color(ssid_lbl, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_align(ssid_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *step2 = lv_label_create(scr);
    lv_label_set_text(step2, "2. Open in browser:");
    lv_obj_set_style_text_color(step2, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(step2, LV_ALIGN_TOP_LEFT, 16, 156);

    lv_obj_t *url = lv_label_create(scr);
    lv_label_set_text(url, "192.168.4.1/wizard");
    lv_obj_set_style_text_color(url, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 180);

    lv_obj_t *waiting = lv_label_create(scr);
    lv_label_set_text(waiting, "waiting for setup...");
    lv_obj_set_style_text_color(waiting, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(waiting, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_scr_load(scr);
    lvgl_port_unlock();
}
