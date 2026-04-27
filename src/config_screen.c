// ---------------------------------------------------------------------------
//  Config screen — see config_screen.h.
//
//  Two accordion sections stacked vertically under the status bar:
//
//    ┌ MODEL: Claude Haiku 4.5     ▼ ┐   ← tap header to expand
//    ├ VOICE: Adam (M)             ▼ ┤
//
//  Expanding one auto-collapses the other so the screen stays tidy.
//  Tapping a row inside a body persists the pick and collapses.
// ---------------------------------------------------------------------------
#include "config_screen.h"
#include "screens_common.h"
#include "devcfg.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "config_screen";

// ---- Catalogs --------------------------------------------------------------

typedef struct { const char *id; const char *name; } catalog_item_t;

// Models routed through the daemon-x402s Vercel facilitator. Provider
// prefix dispatches to /api/openai or /api/anthropic; the suffix is the
// upstream API model id (must match what each provider actually accepts).
static const catalog_item_t MODELS[] = {
    { "anthropic/claude-haiku-4-5",   "Claude Haiku 4.5"   },
    { "anthropic/claude-sonnet-4-6",  "Claude Sonnet 4.6"  },
    { "anthropic/claude-opus-4-7",    "Claude Opus 4.7"    },
    { "openai/gpt-4o-mini",           "GPT-4o Mini"        },
    { "openai/gpt-4o",                "GPT-4o"             },
};

// ElevenLabs stock voices: 3 male + 2 female.
static const catalog_item_t VOICES[] = {
    { "pNInz6obpgDQGcFmaJgB", "Adam (M)"    },
    { "ErXwobaYiN019PkySvjV", "Antoni (M)"  },
    { "TxGEqnHWrfWFTfGW9XjX", "Josh (M)"    },
    { "21m00Tcm4TlvDq8ikWAM", "Rachel (F)"  },
    { "pFZP5JQG7iQjIQuC4Bku", "Lily (F)"    },
};

#define MODEL_COUNT ((int)(sizeof(MODELS) / sizeof(MODELS[0])))
#define VOICE_COUNT ((int)(sizeof(VOICES) / sizeof(VOICES[0])))

// ---- State ------------------------------------------------------------------

static lv_obj_t *s_scr          = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_model_title  = NULL;
static lv_obj_t *s_model_chev   = NULL;
static lv_obj_t *s_model_body   = NULL;
static lv_obj_t *s_voice_title  = NULL;
static lv_obj_t *s_voice_chev   = NULL;
static lv_obj_t *s_voice_body   = NULL;
static lv_obj_t *s_model_rows[MODEL_COUNT];
static lv_obj_t *s_voice_rows[VOICE_COUNT];

// ---- Helpers ----------------------------------------------------------------

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

static int find_idx_by_id(const catalog_item_t *items, int count,
                          const char *id) {
    if (!id || !id[0]) return -1;
    for (int i = 0; i < count; i++) {
        if (!strcmp(items[i].id, id)) return i;
    }
    return -1;
}

// "SECTION: current-name" — shown in the accordion header. Falls back to
// "—" if the saved id isn't in the catalog (e.g. catalog trimmed since
// last save).
static void update_title(lv_obj_t *label, const char *section,
                         const catalog_item_t *items, int count,
                         const char *current_id) {
    int idx = find_idx_by_id(items, count, current_id);
    char buf[72];
    snprintf(buf, sizeof(buf), "%s: %s",
             section, (idx >= 0) ? items[idx].name : "\xE2\x80\x94");
    lv_label_set_text(label, buf);
}

static void repaint_rows(lv_obj_t **rows, int count,
                         const catalog_item_t *items,
                         const char *current_id) {
    int sel = find_idx_by_id(items, count, current_id);
    for (int i = 0; i < count; i++) {
        lv_obj_t *row = rows[i];
        lv_obj_t *lab = lv_obj_get_child(row, 0);
        if (!lab) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s  %s",
                 (i == sel) ? LV_SYMBOL_RIGHT : " ",
                 items[i].name);
        lv_label_set_text(lab, buf);
        lv_obj_set_style_text_color(lab,
            (i == sel) ? SCR_COLOR_ACCENT_HI : SCR_COLOR_TEXT,
            LV_PART_MAIN);
    }
}

static void set_body_expanded(lv_obj_t *body, lv_obj_t *chev, bool expanded) {
    if (expanded) {
        lv_obj_remove_flag(body, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(chev, LV_SYMBOL_UP);
    } else {
        lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(chev, LV_SYMBOL_DOWN);
    }
}

// Debounce repeat clicks — the touch IC occasionally reports a press-
// release flicker during a single hold, which made the accordion look
// like it was opening and closing itself. 300 ms is well below human tap
// cadence but above the jitter window.
#define CLICK_DEBOUNCE_MS 300
static uint32_t s_last_click_ms = 0;

static bool debounce_ok(void) {
    uint32_t now = lv_tick_get();
    if (now - s_last_click_ms < CLICK_DEBOUNCE_MS) return false;
    s_last_click_ms = now;
    return true;
}

static void model_header_clicked(lv_event_t *e) {
    (void)e;
    if (!debounce_ok()) return;
    if (!lvgl_port_lock(0)) return;
    bool will_expand = lv_obj_has_flag(s_model_body, LV_OBJ_FLAG_HIDDEN);
    set_body_expanded(s_model_body, s_model_chev, will_expand);
    if (will_expand) set_body_expanded(s_voice_body, s_voice_chev, false);
    lvgl_port_unlock();
}

static void voice_header_clicked(lv_event_t *e) {
    (void)e;
    if (!debounce_ok()) return;
    if (!lvgl_port_lock(0)) return;
    bool will_expand = lv_obj_has_flag(s_voice_body, LV_OBJ_FLAG_HIDDEN);
    set_body_expanded(s_voice_body, s_voice_chev, will_expand);
    if (will_expand) set_body_expanded(s_model_body, s_model_chev, false);
    lvgl_port_unlock();
}

static void model_row_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MODEL_COUNT) return;
    if (!debounce_ok()) return;
    devcfg_set_llm_model(MODELS[idx].id);
    if (!lvgl_port_lock(0)) return;
    update_title(s_model_title, "MODEL", MODELS, MODEL_COUNT, MODELS[idx].id);
    repaint_rows(s_model_rows, MODEL_COUNT, MODELS, MODELS[idx].id);
    set_body_expanded(s_model_body, s_model_chev, false);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "model set to %s", MODELS[idx].id);
}

static void voice_row_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= VOICE_COUNT) return;
    if (!debounce_ok()) return;
    devcfg_set_voice_id(VOICES[idx].id);
    if (!lvgl_port_lock(0)) return;
    update_title(s_voice_title, "VOICE", VOICES, VOICE_COUNT, VOICES[idx].id);
    repaint_rows(s_voice_rows, VOICE_COUNT, VOICES, VOICES[idx].id);
    set_body_expanded(s_voice_body, s_voice_chev, false);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "voice set to %s", VOICES[idx].id);
}

// A body row — one tappable button with a label child.
static lv_obj_t *make_row(lv_obj_t *parent, const char *name, int idx,
                          lv_event_cb_t cb) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 30);
    lv_obj_set_style_bg_color(btn, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, name);
    lv_obj_set_style_text_color(lab, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(lab, LV_ALIGN_LEFT_MID, 0, 0);
    return btn;
}

// Build one accordion section (header + hidden body) inside `parent`, which
// must already be a flex column. Returns the body so the caller can populate
// it with rows; the title + chevron handles are returned via out params.
static lv_obj_t *make_section(lv_obj_t *parent,
                              const char *section_default_title,
                              lv_event_cb_t header_cb,
                              lv_obj_t **out_title,
                              lv_obj_t **out_chev) {
    lv_obj_t *sec = lv_obj_create(parent);
    lv_obj_set_width(sec, lv_pct(100));
    lv_obj_set_height(sec, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(sec, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sec, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sec, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sec, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(sec, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sec, 0, LV_PART_MAIN);
    lv_obj_set_layout(sec, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(sec, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_button_create(sec);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, 38);
    lv_obj_set_style_bg_color(hdr, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(hdr, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(hdr, header_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, section_default_title);
    lv_obj_set_style_text_color(title, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_width(title, lv_pct(82));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *chev = lv_label_create(hdr);
    lv_label_set_text(chev, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(chev, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *body = lv_obj_create(sec);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(body, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(body, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 2, LV_PART_MAIN);
    lv_obj_set_layout(body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);

    if (out_title) *out_title = title;
    if (out_chev)  *out_chev  = chev;
    return body;
}

// ---- Public API -------------------------------------------------------------

bool config_screen_init(void) {
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
    lv_label_set_text(s_status_label, "config");
    lv_obj_set_style_text_color(s_status_label, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    add_close_button(bar);

    // Scrollable content area under the status bar. When an accordion body
    // expands past the viewport the user scrolls this to see more rows.
    lv_obj_t *content = lv_obj_create(s_scr);
    lv_obj_set_size(content, SCR_W, SCR_H - STATUS_BAR_H);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_H);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 8, LV_PART_MAIN);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    s_model_body = make_section(content, "MODEL", model_header_clicked,
                                &s_model_title, &s_model_chev);
    for (int i = 0; i < MODEL_COUNT; i++) {
        s_model_rows[i] = make_row(s_model_body, MODELS[i].name, i,
                                   model_row_clicked);
    }

    s_voice_body = make_section(content, "VOICE", voice_header_clicked,
                                &s_voice_title, &s_voice_chev);
    for (int i = 0; i < VOICE_COUNT; i++) {
        s_voice_rows[i] = make_row(s_voice_body, VOICES[i].name, i,
                                   voice_row_clicked);
    }

    update_title(s_model_title, "MODEL", MODELS, MODEL_COUNT, devcfg_llm_model());
    update_title(s_voice_title, "VOICE", VOICES, VOICE_COUNT, devcfg_voice_id());
    repaint_rows(s_model_rows, MODEL_COUNT, MODELS, devcfg_llm_model());
    repaint_rows(s_voice_rows, VOICE_COUNT, VOICES, devcfg_voice_id());

    lvgl_port_unlock();
    ESP_LOGI(TAG, "config screen built (%d models, %d voices)",
             MODEL_COUNT, VOICE_COUNT);
    return true;
}

lv_obj_t *config_screen(void) { return s_scr; }

void config_screen_set_status(const char *s) {
    if (!s_status_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_label, s ? s : "");
    lvgl_port_unlock();
}

void config_screen_set_price(const char *s) { (void)s; }
void config_screen_set_usdc (const char *s) { (void)s; }

void config_screen_refresh(void) {
    if (!s_scr) return;
    const char *model = devcfg_llm_model();
    const char *voice = devcfg_voice_id();
    if (!lvgl_port_lock(0)) return;
    update_title(s_model_title, "MODEL", MODELS, MODEL_COUNT, model);
    update_title(s_voice_title, "VOICE", VOICES, VOICE_COUNT, voice);
    repaint_rows(s_model_rows, MODEL_COUNT, MODELS, model);
    repaint_rows(s_voice_rows, VOICE_COUNT, VOICES, voice);
    lvgl_port_unlock();
}
