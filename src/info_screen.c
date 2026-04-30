// ---------------------------------------------------------------------------
//  Info screen — see info_screen.h for the contract.
// ---------------------------------------------------------------------------
#include "info_screen.h"
#include "screens_common.h"
#include "ui.h"
#include "wifi_sta.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_mac.h"
#include "esp_timer.h"

static const char *TAG = "info_screen";

static lv_obj_t *s_scr          = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_label_fw     = NULL;
static lv_obj_t *s_label_idf    = NULL;
static lv_obj_t *s_label_chip   = NULL;
static lv_obj_t *s_label_mac    = NULL;
static lv_obj_t *s_label_ip     = NULL;
static lv_obj_t *s_label_heap   = NULL;
static lv_obj_t *s_label_uptime = NULL;

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

// Create one "key: value" pair in a 2-line slot. Key is dim, value is the
// standard text colour. Returns the value label so the caller can keep a
// handle for future updates.
static lv_obj_t *add_info_row(lv_obj_t *parent, int y_top,
                              const char *key, const char *initial_value) {
    lv_obj_t *k = lv_label_create(parent);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, 12, y_top);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, initial_value);
    lv_obj_set_style_text_color(v, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_width(v, SCR_W - 24);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, 12, y_top + 14);
    return v;
}

bool info_screen_init(void) {
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
    lv_label_set_text(s_status_label, "info");
    lv_obj_set_style_text_color(s_status_label, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    add_close_button(bar);

    // Seven rows at 34 px each fit within 320 - 26 status bar = 294 px.
    int y = STATUS_BAR_H + 8;
    const int row_h = 34;
    s_label_fw     = add_info_row(s_scr, y, "Firmware",  "?"); y += row_h;
    s_label_idf    = add_info_row(s_scr, y, "IDF",       "?"); y += row_h;
    s_label_chip   = add_info_row(s_scr, y, "Chip",      "?"); y += row_h;
    s_label_mac    = add_info_row(s_scr, y, "MAC",       "?"); y += row_h;
    s_label_ip     = add_info_row(s_scr, y, "IP",        "?"); y += row_h;
    s_label_heap   = add_info_row(s_scr, y, "Heap",      "?"); y += row_h;
    s_label_uptime = add_info_row(s_scr, y, "Uptime",    "?");

    lvgl_port_unlock();

    info_screen_refresh();

    ESP_LOGI(TAG, "info screen built");
    return true;
}

lv_obj_t *info_screen(void) { return s_scr; }

void info_screen_destroy(void) {
    if (!s_scr) return;
    if (!lvgl_port_lock(0)) return;
    lv_obj_delete(s_scr);
    s_status_label = NULL;
    s_label_fw     = NULL;
    s_label_idf    = NULL;
    s_label_chip   = NULL;
    s_label_mac    = NULL;
    s_label_ip     = NULL;
    s_label_heap   = NULL;
    s_label_uptime = NULL;
    s_scr          = NULL;
    lvgl_port_unlock();
}

void info_screen_set_status(const char *s) {
    if (!s_status_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_label, s ? s : "");
    lvgl_port_unlock();
}

void info_screen_set_price(const char *s) { (void)s; }
void info_screen_set_usdc (const char *s) { (void)s; }

void info_screen_refresh(void) {
    if (!s_scr) return;

    // Gather everything under local buffers first so we hold the LVGL
    // port lock only during the label writes. MAC, chip info, and IDF
    // version are constants — we re-read MAC/chip on every refresh
    // because the cost is trivial and it keeps the logic uniform.
    const esp_app_desc_t *app = esp_app_get_description();

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char ip[16] = {0};
    wifi_sta_ip_str(ip, sizeof(ip));

    size_t heap_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
    uint32_t h = (uint32_t)(uptime_s / 3600);
    uint32_t m = (uint32_t)((uptime_s % 3600) / 60);
    uint32_t s = (uint32_t)(uptime_s % 60);

    char fw_line[64], idf_line[32], chip_line[48], mac_line[32];
    char ip_line[32], heap_line[64], uptime_line[32];

    snprintf(fw_line,  sizeof(fw_line),  "%s  %s", app->version, app->date);
    snprintf(idf_line, sizeof(idf_line), "%s", app->idf_ver);
    snprintf(chip_line, sizeof(chip_line), "%s rev v%d.%d  %dc",
             CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100,
             chip.cores);
    snprintf(mac_line, sizeof(mac_line), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(ip_line,  sizeof(ip_line),  "%s", ip[0] ? ip : "(none)");
    snprintf(heap_line, sizeof(heap_line), "int %u KB  psram %u KB",
             (unsigned)(heap_internal / 1024),
             (unsigned)(heap_psram    / 1024));
    snprintf(uptime_line, sizeof(uptime_line), "%" PRIu32 "h %" PRIu32 "m %" PRIu32 "s",
             h, m, s);

    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_label_fw,     fw_line);
    lv_label_set_text(s_label_idf,    idf_line);
    lv_label_set_text(s_label_chip,   chip_line);
    lv_label_set_text(s_label_mac,    mac_line);
    lv_label_set_text(s_label_ip,     ip_line);
    lv_label_set_text(s_label_heap,   heap_line);
    lv_label_set_text(s_label_uptime, uptime_line);
    lvgl_port_unlock();
}
