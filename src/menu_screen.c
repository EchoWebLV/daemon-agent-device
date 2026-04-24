// ---------------------------------------------------------------------------
//  Menu screen implementation — see menu_screen.h for the UI contract.
// ---------------------------------------------------------------------------
#include "menu_screen.h"
#include "screens_common.h"
#include "ui.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "menu_screen";

static lv_obj_t *s_scr          = NULL;
static lv_obj_t *s_status_label = NULL;

// Same "X" handler pattern used by the other side screens. Duplicated
// locally (rather than pulled out into screens_common.h) for the same
// reason settings_screen.c keeps its copy local — the one-file traversal
// to read the handler is cheaper than the include chain.
static void close_clicked(lv_event_t *e) {
    (void)e;
    ui_show_creature();
}

static void add_close_button(lv_obj_t *bar) {
    lv_obj_t *btn = lv_button_create(bar);
    lv_obj_set_size(btn, 28, STATUS_BAR_H - 4);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, close_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *x = lv_label_create(btn);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_center(x);
}

// Row click handlers — one per destination. ui_show_* is safe from LVGL
// event context (it takes the port lock internally, which is re-entrant).
static void row_wallet_clicked(lv_event_t *e) { (void)e; ui_show_wallet(); }
static void row_info_clicked  (lv_event_t *e) { (void)e; ui_show_info();   }
static void row_config_clicked(lv_event_t *e) { (void)e; ui_show_config(); }

// Build a single 56-px tall tappable row with a title + ">" chevron.
static lv_obj_t *make_menu_row(lv_obj_t *parent, int y_top,
                               const char *title,
                               lv_event_cb_t click_cb) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, SCR_W - 24, 56);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 12, y_top);
    lv_obj_set_style_bg_color(row, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 12, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *chev = lv_label_create(row);
    lv_label_set_text(chev, ">");
    lv_obj_set_style_text_color(chev, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);

    return row;
}

bool menu_screen_init(void) {
    if (s_scr) return true;
    if (!lvgl_port_lock(0)) { ESP_LOGE(TAG, "lvgl_port_lock failed"); return false; }

    s_scr = lv_obj_create(NULL);
    scr_apply_bg(s_scr);

    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, SCR_W, STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 4, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(bar);
    lv_label_set_text(s_status_label, "menu");
    lv_obj_set_style_text_color(s_status_label, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    add_close_button(bar);

    int y = STATUS_BAR_H + 16;
    make_menu_row(s_scr, y, "Wallet", row_wallet_clicked); y += 64;
    make_menu_row(s_scr, y, "Info",   row_info_clicked);   y += 64;
    make_menu_row(s_scr, y, "Config", row_config_clicked);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "menu screen built");
    return true;
}

lv_obj_t *menu_screen(void) { return s_scr; }

void menu_screen_set_status(const char *s) {
    if (!s_status_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_label, s ? s : "");
    lvgl_port_unlock();
}

void menu_screen_set_price(const char *s) { (void)s; }
void menu_screen_set_usdc (const char *s) { (void)s; }
