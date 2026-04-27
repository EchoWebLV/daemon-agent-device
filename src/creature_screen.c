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
//    |                                                  |
//    |    ███       ███     <- eyes (30×30, blink)      |
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
#include "mic.h"
#include "stt.h"
#include "ai.h"
#include "ui.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"

static const char *TAG = "creature_screen";

// Pixel size — base unit for every face feature.
#define PX              10
#define EYE_W           30
#define EYE_H           30
#define MOUTH_W         60
// Face geometry recentered for 320×240 landscape on the BOX-3:
//   - shift X by +40 (extra horizontal room from the 240→320 panel)
//   - shift Y by -40 (less vertical room from the 320→240 panel)
// Result: eyes around top third, smile around lower third, all centered.
#define EYE_L_X         115
#define EYE_R_X        (SCR_W - EYE_L_X - EYE_W)
#define EYE_Y           75

// Shift every face block down by this much from the coordinates listed in
// the k_* tables below. Tweak in one place to raise/lower the face without
// touching each feature's Y column.
#define FACE_Y_OFFSET   (-5)
#define SMILE_MID_Y    (165 + FACE_Y_OFFSET)    // where the talking mouth centers

// --- widget handles --------------------------------------------------------
static lv_obj_t *s_scr          = NULL;
static lv_obj_t *s_price_label  = NULL;
static lv_obj_t *s_usdc_label   = NULL;

static lv_obj_t *s_eye_l        = NULL;
static lv_obj_t *s_eye_r        = NULL;

// Talking mouth — 6 cols × 3 rows of 10×10 pixel blocks. Each frame toggles
// a subset visible to build up a sprite. Blocks are pre-created hidden and
// only flipped via LV_OBJ_FLAG_HIDDEN at runtime so no widgets churn.
#define MOUTH_COLS      6
#define MOUTH_ROWS      3
static lv_obj_t *s_mouth_grid[MOUTH_ROWS][MOUTH_COLS] = {0};

static lv_obj_t *s_smile[14]    = {0};   // 14-pixel smile shown while idle
static lv_obj_t *s_subtitle     = NULL;

// Subtitle pagination — long replies get split into ≤2-line pages and
// rotated through on a timer so the viewer can read along instead of
// being hit with the whole wall of text. Budget of 52 chars fits two
// wrapped lines of the default Montserrat 14 font at a 216 px label width;
// 3200 ms dwell lands roughly in step with ElevenLabs' speaking cadence.
#define SUB_PAGE_CHARS      52
#define SUB_MAX_PAGES       12
#define SUB_PAGE_MS         3200
static char        s_sub_pages[SUB_MAX_PAGES][SUB_PAGE_CHARS + 1];
static int         s_sub_page_count = 0;
static int         s_sub_page_idx   = 0;
static lv_timer_t *s_sub_timer      = NULL;

// Pixel coordinates for static face features.
static const int16_t k_smile[14][2] = {
    // X +40, Y -40 from the original 240×320 design — see EYE_L_X/EYE_Y notes.
    // row 0 — outermost rising tips
    { 90, 145}, {220, 145},
    // row 1 — inner shoulders one column in
    {100, 155}, {210, 155},
    // row 2 — base of smile arc (10 pixels wide)
    {110, 165}, {120, 165}, {130, 165}, {140, 165},
    {150, 165}, {160, 165}, {170, 165}, {180, 165},
    {190, 165}, {200, 165},
};

// --- mood + talking state --------------------------------------------------
static creature_mood_t s_mood    = CREATURE_MOOD_IDLE;
static bool            s_talking = false;

// --- talking mouth: sprite frames ------------------------------------------
// Three visually distinct sprites — each row-major within a 6×3 bounding box
// with 1 = pixel visible. The silhouettes are chosen so frame transitions
// read as discrete sprite swaps (flat bar → hollow O → filled block) rather
// than a bar growing in height.
static const uint8_t MOUTH_FRAMES[3][MOUTH_ROWS][MOUTH_COLS] = {
    // F0 — closed (horizontal bar across the middle row)
    { {0,0,0,0,0,0},
      {1,1,1,1,1,1},
      {0,0,0,0,0,0} },
    // F1 — "o" (hollow ring — top + bottom arc, side pixels)
    { {0,1,1,1,1,0},
      {1,0,0,0,0,1},
      {0,1,1,1,1,0} },
    // F2 — wide open (filled block)
    { {1,1,1,1,1,1},
      {1,1,1,1,1,1},
      {1,1,1,1,1,1} },
};
// Cycle through the sprites without bouncing — each frame is a hard swap
// into a different silhouette so the animation doesn't read as a bar sliding.
static const uint8_t MOUTH_CYCLE[] = {0, 1, 2, 1, 0, 2};
#define MOUTH_FRAME_MS      150

static lv_timer_t *s_mouth_timer = NULL;
static int         s_mouth_cyc   = 0;

// --- idle blink state machine ----------------------------------------------
// Each blink is a sequence of frames describing which eyes are closed and
// for how long. Frame with ms == 0 is the terminator. Between sequences the
// timer idles for blink_wait_ms(), which is randomised so blinks don't feel
// mechanical. A weighted roll at the idle boundary picks the next type:
// most blinks are single; occasional doubles and single-eye winks mix it up.
#define EYE_CLOSED_L    0x1
#define EYE_CLOSED_R    0x2
#define EYE_CLOSED_BOTH (EYE_CLOSED_L | EYE_CLOSED_R)

typedef struct { uint8_t eyes; uint16_t ms; } blink_frame_t;

static const blink_frame_t BLINK_SINGLE[] = {
    { EYE_CLOSED_BOTH, 120 },
    { 0, 0 },
};
static const blink_frame_t BLINK_DOUBLE[] = {
    { EYE_CLOSED_BOTH, 90 },
    { 0,                90 },
    { EYE_CLOSED_BOTH, 90 },
    { 0, 0 },
};
static const blink_frame_t BLINK_WINK_L[] = {
    { EYE_CLOSED_L, 220 },
    { 0, 0 },
};
static const blink_frame_t BLINK_WINK_R[] = {
    { EYE_CLOSED_R, 220 },
    { 0, 0 },
};

static lv_timer_t          *s_blink_timer = NULL;
static const blink_frame_t *s_blink_seq   = NULL;  // NULL = idling between blinks
static int                  s_blink_step  = 0;

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

// --- talking mouth painters ------------------------------------------------

static void mouth_paint_frame(int frame) {
    if (frame < 0 || frame >= (int)(sizeof(MOUTH_FRAMES)/sizeof(MOUTH_FRAMES[0]))) return;
    for (int r = 0; r < MOUTH_ROWS; r++) {
        for (int c = 0; c < MOUTH_COLS; c++) {
            lv_obj_t *p = s_mouth_grid[r][c];
            if (!p) continue;
            if (MOUTH_FRAMES[frame][r][c]) lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
            else                           lv_obj_add_flag   (p, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void mouth_hide_all(void) {
    for (int r = 0; r < MOUTH_ROWS; r++) {
        for (int c = 0; c < MOUTH_COLS; c++) {
            if (s_mouth_grid[r][c]) lv_obj_add_flag(s_mouth_grid[r][c], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void mouth_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_talking) return;
    mouth_paint_frame(MOUTH_CYCLE[s_mouth_cyc]);
    s_mouth_cyc = (s_mouth_cyc + 1) % (sizeof(MOUTH_CYCLE)/sizeof(MOUTH_CYCLE[0]));
}

// Show/hide the static smile block.
static void set_smile_visible(bool visible) {
    for (size_t i = 0; i < sizeof(s_smile) / sizeof(s_smile[0]); i++) {
        if (!s_smile[i]) continue;
        if (visible) lv_obj_remove_flag(s_smile[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag   (s_smile[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// --- blink painters --------------------------------------------------------

// Resize + reposition each eye so a "closed" eye renders as a 10-px slit
// at the bottom edge of where the open eye was. Snapping between the two
// extremes (no tweening) keeps the motion feeling pixel-art.
static void blink_paint(uint8_t eyes) {
    int16_t open_y   = EYE_Y + FACE_Y_OFFSET;
    int16_t closed_y = EYE_Y + FACE_Y_OFFSET + EYE_H - PX;
    bool lc = (eyes & EYE_CLOSED_L) != 0;
    bool rc = (eyes & EYE_CLOSED_R) != 0;
    if (s_eye_l) {
        lv_obj_set_size(s_eye_l, EYE_W, lc ? PX : EYE_H);
        lv_obj_set_pos (s_eye_l, EYE_L_X, lc ? closed_y : open_y);
    }
    if (s_eye_r) {
        lv_obj_set_size(s_eye_r, EYE_W, rc ? PX : EYE_H);
        lv_obj_set_pos (s_eye_r, EYE_R_X, rc ? closed_y : open_y);
    }
}

static uint32_t blink_wait_ms(void) {
    // 2500–6500 ms gap between blinks.
    return 2500u + (esp_random() % 4000u);
}

static void blink_pick_next(void) {
    uint32_t r = esp_random() % 100u;
    if      (r < 55u) s_blink_seq = BLINK_SINGLE;   // most of the time: a normal blink
    else if (r < 80u) s_blink_seq = BLINK_DOUBLE;   // occasional double
    else if (r < 90u) s_blink_seq = BLINK_WINK_L;   // sometimes a left wink
    else              s_blink_seq = BLINK_WINK_R;   // rarely a right wink
    s_blink_step = 0;
    blink_paint(s_blink_seq[0].eyes);
    if (s_blink_timer) lv_timer_set_period(s_blink_timer, s_blink_seq[0].ms);
}

static void blink_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_blink_seq) { blink_pick_next(); return; }
    s_blink_step++;
    uint16_t ms = s_blink_seq[s_blink_step].ms;
    if (ms == 0) {
        // End of sequence — eyes back open, schedule next blink.
        s_blink_seq = NULL;
        blink_paint(0);
        if (s_blink_timer) lv_timer_set_period(s_blink_timer, blink_wait_ms());
        return;
    }
    blink_paint(s_blink_seq[s_blink_step].eyes);
    if (s_blink_timer) lv_timer_set_period(s_blink_timer, ms);
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

    // --- eyes: 30×30 blocks (no rounded corners — pixel aesthetic) -------
    // Liveness comes from the blink state machine rather than opacity pulse,
    // so no install_eye_pulse() here.
    s_eye_l = make_block(s_scr, EYE_L_X, EYE_Y + FACE_Y_OFFSET, EYE_W, EYE_H, face);
    s_eye_r = make_block(s_scr, EYE_R_X, EYE_Y + FACE_Y_OFFSET, EYE_W, EYE_H, face);

    // --- smile: 14 pixel blocks forming a wide curved-upward arc ---------
    for (size_t i = 0; i < sizeof(s_smile) / sizeof(s_smile[0]); i++) {
        s_smile[i] = make_block(s_scr, k_smile[i][0],
                                k_smile[i][1] + FACE_Y_OFFSET, PX, PX, face);
    }

    // --- talking mouth grid: 6×3 pixel sprite, hidden until talking ------
    {
        int origin_x = (SCR_W - MOUTH_COLS * PX) / 2;
        int origin_y = SMILE_MID_Y - (MOUTH_ROWS * PX) / 2;
        for (int r = 0; r < MOUTH_ROWS; r++) {
            for (int c = 0; c < MOUTH_COLS; c++) {
                lv_obj_t *p = make_block(s_scr,
                                         origin_x + c * PX,
                                         origin_y + r * PX,
                                         PX, PX, face);
                lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
                s_mouth_grid[r][c] = p;
            }
        }
    }

    s_mouth_timer = lv_timer_create(mouth_timer_cb, MOUTH_FRAME_MS, NULL);
    lv_timer_pause(s_mouth_timer);

    // --- blink timer: idle-wait until first blink, then state machine ----
    s_blink_timer = lv_timer_create(blink_timer_cb, blink_wait_ms(), NULL);

    // --- subtitle ---------------------------------------------------------
    s_subtitle = lv_label_create(s_scr);
    lv_label_set_text(s_subtitle, "");
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_subtitle, SCR_W - 24);
    lv_obj_set_style_text_align(s_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_subtitle, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_subtitle, LV_ALIGN_BOTTOM_MID, 0, -8);

    // --- Push-to-talk: long-press anywhere on the face to record ----------
    // LV_EVENT_LONG_PRESSED fires once after the default ~400 ms hold; we
    // start mic capture there and stop on RELEASED / PRESS_LOST. The
    // recording path lives entirely in mic.c + stt.c — this handler is
    // just the UI trigger.
    extern void creature_screen_on_long_press_(struct _lv_event_t *e);
    extern void creature_screen_on_press_end_(struct _lv_event_t *e);
    lv_obj_add_event_cb(s_scr, creature_screen_on_long_press_,
                        LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr, creature_screen_on_press_end_,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_scr, creature_screen_on_press_end_,
                        LV_EVENT_PRESS_LOST, NULL);

    // Apply the initial mood tint.
    creature_mood_t m = s_mood;
    s_mood = (creature_mood_t)-1;   // force set path on first call
    lvgl_port_unlock();
    creature_screen_set_mood(m);

    ESP_LOGI(TAG, "creature screen built (pixel face)");
    return true;
}

// --- Push-to-talk handlers --------------------------------------------------

static volatile bool s_speech_in_flight = false;

static void speech_task(void *arg) {
    int16_t *pcm    = (int16_t *)arg;
    size_t   frames = 0;
    // The mic_record_stop call below has already been executed on the LVGL
    // thread; we received the buffer + frame count via two static slots
    // because lv_async_call only takes one void* and FreeRTOS task args are
    // a single pointer. Frames are stored as the size_t suffix of the alloc
    // header — simpler to just stash in a static. See on_press_end_.
    extern volatile size_t s_speech_frames;
    frames = s_speech_frames;
    s_speech_frames = 0;

    char transcript[512] = {0};
    bool got_text = stt_transcribe(pcm, frames, transcript, sizeof(transcript));
    mic_buffer_free(pcm);

    if (!got_text || !transcript[0]) {
        ESP_LOGW(TAG, "STT returned no text — telling the user");
        ui_set_subtitle("I didn't catch that.");
        s_speech_in_flight = false;
        vTaskDelete(NULL);
        return;
    }

    // Surface the transcript on screen so the user can sanity-check what
    // Whisper heard before the AI reply overwrites it. Stays visible
    // until ai_handle_say returns and ui_deliver_reply replaces it.
    char displayed[600];
    snprintf(displayed, sizeof(displayed), "you: %s", transcript);
    ui_set_subtitle(displayed);
    ESP_LOGI(TAG, "transcript surfaced: %s", transcript);

    // Mirror the typed-input path: ai_handle_say + ui_deliver_reply.
    char reply[1024] = {0};
    ai_handle_say(transcript, reply, sizeof(reply));
    if (reply[0]) {
        ui_deliver_reply(reply);
    }
    s_speech_in_flight = false;
    vTaskDelete(NULL);
}

volatile size_t s_speech_frames = 0;

void creature_screen_on_long_press_(struct _lv_event_t *e) {
    (void)e;
    if (s_speech_in_flight || mic_is_recording()) return;
    if (mic_record_start() == ESP_OK) {
        ui_set_subtitle("Listening...");
    } else {
        ESP_LOGW(TAG, "mic_record_start failed");
    }
}

void creature_screen_on_press_end_(struct _lv_event_t *e) {
    (void)e;
    if (!mic_is_recording()) return;
    int16_t *pcm    = NULL;
    size_t   frames = 0;
    esp_err_t err = mic_record_stop(&pcm, &frames);
    if (err != ESP_OK || !pcm || frames < 1600) {   // <100 ms = junk
        if (pcm) mic_buffer_free(pcm);
        ui_set_subtitle("");
        return;
    }
    ui_set_subtitle("Thinking...");
    s_speech_frames = frames;
    s_speech_in_flight = true;
    // 16 KB stack: ai_handle_say does HTTPS via mbedTLS (handshake eats
    // 5-8 KB) + the 1 KB reply buffer + STT scratch — 8 KB overflowed.
    BaseType_t ok = xTaskCreatePinnedToCore(
        speech_task, "speech", 16384, pcm, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "speech_task spawn failed");
        mic_buffer_free(pcm);
        s_speech_in_flight = false;
        ui_set_subtitle("");
    }
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

// Break `text` into ≤ SUB_MAX_PAGES chunks of ≤ SUB_PAGE_CHARS. Breaks on
// the last space inside each window; falls back to a hard cut when a single
// word exceeds the page budget. Trailing spaces are stripped so the visible
// line doesn't end in weird whitespace.
static void sub_split_pages(const char *text) {
    s_sub_page_count = 0;
    if (!text) return;
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len && s_sub_page_count < SUB_MAX_PAGES) {
        while (i < len && text[i] == ' ') i++;
        if (i >= len) break;
        size_t remaining = len - i;
        size_t take;
        if (remaining <= SUB_PAGE_CHARS) {
            take = remaining;
        } else {
            size_t k = i + SUB_PAGE_CHARS;
            while (k > i && text[k] != ' ') k--;
            take = (k == i) ? SUB_PAGE_CHARS : (k - i);
        }
        size_t copy = take;
        while (copy > 0 && text[i + copy - 1] == ' ') copy--;
        if (copy > SUB_PAGE_CHARS) copy = SUB_PAGE_CHARS;
        memcpy(s_sub_pages[s_sub_page_count], text + i, copy);
        s_sub_pages[s_sub_page_count][copy] = '\0';
        s_sub_page_count++;
        i += take;
    }
}

static void sub_show_page(int idx) {
    if (!s_subtitle || idx < 0 || idx >= s_sub_page_count) return;
    lv_label_set_text(s_subtitle, s_sub_pages[idx]);
}

// Advance to the next page; freeze on the last one so the tail of the
// message stays on screen until the next subtitle arrives.
static void sub_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_sub_page_count == 0) return;
    if (s_sub_page_idx + 1 >= s_sub_page_count) {
        if (s_sub_timer) lv_timer_pause(s_sub_timer);
        return;
    }
    s_sub_page_idx++;
    sub_show_page(s_sub_page_idx);
}

void creature_screen_set_subtitle(const char *text) {
    if (!s_subtitle) return;
    if (!lvgl_port_lock(0)) return;

    if (s_sub_timer) lv_timer_pause(s_sub_timer);
    s_sub_page_count = 0;
    s_sub_page_idx   = 0;

    if (!text || !text[0]) {
        lv_label_set_text(s_subtitle, "");
        lvgl_port_unlock();
        return;
    }

    sub_split_pages(text);
    if (s_sub_page_count == 0) {
        // Degenerate split (shouldn't hit, but fall back to raw text).
        lv_label_set_text(s_subtitle, text);
        lvgl_port_unlock();
        return;
    }

    sub_show_page(0);

    if (s_sub_page_count > 1) {
        if (!s_sub_timer) {
            s_sub_timer = lv_timer_create(sub_timer_cb, SUB_PAGE_MS, NULL);
        } else {
            lv_timer_set_period(s_sub_timer, SUB_PAGE_MS);
            lv_timer_resume(s_sub_timer);
        }
        lv_timer_reset(s_sub_timer);
    }

    lvgl_port_unlock();
}

void creature_screen_set_mood(creature_mood_t m) {
    if (!s_scr) return;
    if (m == s_mood) return;
    s_mood = m;

    lv_color_t col = mood_color(m);

    if (!lvgl_port_lock(0)) return;
    // Repaint every face block so the whole creature reads the same mood.
    if (s_eye_l) lv_obj_set_style_bg_color(s_eye_l, col, LV_PART_MAIN);
    if (s_eye_r) lv_obj_set_style_bg_color(s_eye_r, col, LV_PART_MAIN);
    for (size_t i = 0; i < sizeof(s_smile) / sizeof(s_smile[0]); i++) {
        if (s_smile[i]) lv_obj_set_style_bg_color(s_smile[i], col, LV_PART_MAIN);
    }
    for (int r = 0; r < MOUTH_ROWS; r++) {
        for (int c = 0; c < MOUTH_COLS; c++) {
            if (s_mouth_grid[r][c])
                lv_obj_set_style_bg_color(s_mouth_grid[r][c], col, LV_PART_MAIN);
        }
    }
    lvgl_port_unlock();
}

void creature_screen_set_talking(bool on) {
    if (s_talking == on) return;
    s_talking = on;

    if (!lvgl_port_lock(0)) return;
    if (on) {
        // Hide static smile and reveal the animated sprite. Timer drives frames.
        set_smile_visible(false);
        s_mouth_cyc = 0;
        mouth_paint_frame(MOUTH_CYCLE[0]);
        if (s_mouth_timer) lv_timer_resume(s_mouth_timer);
    } else {
        if (s_mouth_timer) lv_timer_pause(s_mouth_timer);
        mouth_hide_all();
        set_smile_visible(true);
    }
    lvgl_port_unlock();
}

// Mouth and blink animation both run off lv_timers inside the LVGL task, so
// this hook has nothing to do. Retained so existing integration points keep
// compiling and can poke a frame between ticks if they ever need to.
void creature_screen_tick(void) {
}

// ---- shake ------------------------------------------------------------------
// Jitters the whole screen root via translate_x. Runs off an lv_timer so the
// animation is driven on the LVGL task with no external scheduling. Frame
// cadence (40 ms) and step count (24) give ~960 ms of shake with a ramp-down
// tail so the motion settles instead of snapping back to centre.
#define SHAKE_FRAME_MS   40
#define SHAKE_FRAMES     24
#define SHAKE_FADE_AT    18
#define SHAKE_AMPLITUDE  10

static lv_timer_t *s_shake_timer = NULL;
static int         s_shake_frame = 0;

static void shake_cb(lv_timer_t *t) {
    if (!s_scr) { lv_timer_delete(t); s_shake_timer = NULL; return; }
    s_shake_frame++;
    if (s_shake_frame >= SHAKE_FRAMES) {
        lv_obj_set_style_translate_x(s_scr, 0, LV_PART_MAIN);
        lv_timer_delete(t);
        s_shake_timer = NULL;
        return;
    }
    int amp = SHAKE_AMPLITUDE;
    if (s_shake_frame >= SHAKE_FADE_AT) {
        amp = SHAKE_AMPLITUDE * (SHAKE_FRAMES - s_shake_frame) / (SHAKE_FRAMES - SHAKE_FADE_AT);
    }
    int offset = (s_shake_frame & 1) ? amp : -amp;
    lv_obj_set_style_translate_x(s_scr, offset, LV_PART_MAIN);
}

void creature_screen_shake(void) {
    if (!s_scr) return;
    if (!lvgl_port_lock(0)) return;
    if (!s_shake_timer) {
        s_shake_frame = 0;
        s_shake_timer = lv_timer_create(shake_cb, SHAKE_FRAME_MS, NULL);
    }
    lvgl_port_unlock();
}
