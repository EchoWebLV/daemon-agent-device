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
#include "devcfg.h"
#include "mic.h"
#include "stt.h"
#include "ai.h"
#include "ui.h"
#include "voice.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

// --- creature trait variants -----------------------------------------------
// Three creatures, each a distinct combination of eye / mouth / brow traits.
// Index 0 is the original Daemon — unchanged from before this feature
// landed (round eyes, full smile, no brow). Sliding LEFT/RIGHT on the
// creature screen cycles through them; the active index persists in NVS
// via devcfg_creature_index.
//
// Adding a 4th creature:  bump CREATURE_COUNT in devcfg.c, append a row
// here, and add coord rows to the eye / mouth / brow tables below.
#define CREATURE_COUNT          3

typedef enum { EYE_ROUND = 0, EYE_SLIT = 1, EYE_BIG = 2 } eye_variant_t;
typedef enum { MOUTH_SMILE = 0, MOUTH_SMIRK = 1, MOUTH_FLAT = 2 } mouth_variant_t;
typedef enum { BROW_NONE = 0, BROW_ANGRY = 1, BROW_RAISED = 2 } brow_variant_t;

typedef struct {
    eye_variant_t   eye;
    mouth_variant_t mouth;
    brow_variant_t  brow;
} creature_traits_t;

static const creature_traits_t CREATURES[CREATURE_COUNT] = {
    /* 0 — Daemon (original): round eyes, full smile, no brow                */
    { .eye = EYE_ROUND, .mouth = MOUTH_SMILE, .brow = BROW_NONE   },
    /* 1 — slit eyes (laser-eye vibe), smirk, angry slanted brows            */
    { .eye = EYE_SLIT,  .mouth = MOUTH_SMIRK, .brow = BROW_ANGRY  },
    /* 2 — big eyes with pupils, flat-line mouth, raised arched brows        */
    { .eye = EYE_BIG,   .mouth = MOUTH_FLAT,  .brow = BROW_RAISED },
};

// Active variant — initialised from NVS in init(), updated by cycle().
static int s_creature_idx = 0;

// Eye geometry for the active variant. Set by apply_eye_variant() and
// read by blink_paint() so blinks shrink to the right shape regardless of
// which creature is wearing them.
static int16_t s_eye_open_w  = EYE_W;
static int16_t s_eye_open_h  = EYE_H;
static int16_t s_eye_open_y  = EYE_Y + FACE_Y_OFFSET;
static int16_t s_eye_l_x     = EYE_L_X;
static int16_t s_eye_r_x     = EYE_R_X;

// Variant-only widgets, all created in init() and shown/hidden as the
// active creature changes. Brow blocks are kept around the whole time
// and just repositioned per brow variant — saves churning widgets.
static lv_obj_t *s_pupil_l        = NULL;
static lv_obj_t *s_pupil_r        = NULL;
#define BROW_BLOCKS_PER_SIDE  3
static lv_obj_t *s_brow_l[BROW_BLOCKS_PER_SIDE] = {0};
static lv_obj_t *s_brow_r[BROW_BLOCKS_PER_SIDE] = {0};

// Read by blink_paint (defined further down) and set by apply_eye_variant.
// Hoisted here so blink_paint can reference it — without the forward
// declaration the file wouldn't compile.
static bool s_pupils_present = false;

// Geometry tables for variant-only blocks. All coordinates use
// FACE_Y_OFFSET like the rest of the face so the whole creature shifts
// together if we ever re-centre.

// 3 blocks per brow side. Y values per variant; X stays the same so the
// brows sit directly above each eye column (left eye spans X 115..145,
// right eye spans X 175..205, both 30 wide in ROUND).
static const int16_t k_brow_x_l[BROW_BLOCKS_PER_SIDE] = {115, 125, 135};
static const int16_t k_brow_x_r[BROW_BLOCKS_PER_SIDE] = {175, 185, 195};

// ANGRY: outer-end UP, inner-end DOWN — slants point toward the nose.
static const int16_t k_brow_y_angry_l[BROW_BLOCKS_PER_SIDE]  = {45, 50, 55};
static const int16_t k_brow_y_angry_r[BROW_BLOCKS_PER_SIDE]  = {55, 50, 45};
// RAISED: arch shape — sides low, middle high.
static const int16_t k_brow_y_raised_l[BROW_BLOCKS_PER_SIDE] = {55, 45, 55};
static const int16_t k_brow_y_raised_r[BROW_BLOCKS_PER_SIDE] = {55, 45, 55};

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

// Resize + reposition each eye so a "closed" eye renders as a thin slit
// at the bottom edge of where the open eye was. Snapping between the two
// extremes (no tweening) keeps the motion feeling pixel-art. Geometry
// tracked in s_eye_open_* so blinks adapt to whichever variant is active.
static void blink_paint(uint8_t eyes) {
    int16_t open_y   = s_eye_open_y;
    int16_t closed_y = s_eye_open_y + s_eye_open_h - PX;
    bool lc = (eyes & EYE_CLOSED_L) != 0;
    bool rc = (eyes & EYE_CLOSED_R) != 0;
    if (s_eye_l) {
        lv_obj_set_size(s_eye_l, s_eye_open_w, lc ? PX : s_eye_open_h);
        lv_obj_set_pos (s_eye_l, s_eye_l_x,    lc ? closed_y : open_y);
    }
    if (s_eye_r) {
        lv_obj_set_size(s_eye_r, s_eye_open_w, rc ? PX : s_eye_open_h);
        lv_obj_set_pos (s_eye_r, s_eye_r_x,    rc ? closed_y : open_y);
    }
    // Pupils ride on top of the eyes. Two gates: the variant has to want
    // them (s_pupils_present), AND the eye has to be open. Without the
    // first gate, blink_paint(0) would un-hide pupils for variants like
    // EYE_ROUND that aren't supposed to have them, leaking "eye holes"
    // onto creature 0 the first time the blink machine ran.
    if (s_pupil_l) {
        if (s_pupils_present && !lc) lv_obj_remove_flag(s_pupil_l, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_add_flag   (s_pupil_l, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pupil_r) {
        if (s_pupils_present && !rc) lv_obj_remove_flag(s_pupil_r, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_add_flag   (s_pupil_r, LV_OBJ_FLAG_HIDDEN);
    }
}

// --- variant appliers ------------------------------------------------------
//
// Each takes the variant index and reshapes the relevant face widgets in
// place. Caller holds lvgl_port_lock. None allocate or free widgets — all
// variant geometry is achieved by resize + reposition + visibility flags
// on widgets created up front in init().

static void apply_eye_variant(eye_variant_t v) {
    switch (v) {
    case EYE_ROUND:
    default:
        s_eye_open_w = EYE_W;
        s_eye_open_h = EYE_H;
        s_eye_open_y = EYE_Y + FACE_Y_OFFSET;
        s_eye_l_x    = EYE_L_X;
        s_eye_r_x    = EYE_R_X;
        s_pupils_present = false;
        break;
    case EYE_SLIT:
        // Wide horizontal slit — 40 wide × 10 tall, centred vertically in
        // the original eye box (Y 70..100 → centred Y 80, height 10).
        s_eye_open_w = 40;
        s_eye_open_h = 10;
        s_eye_open_y = 80 + FACE_Y_OFFSET;
        s_eye_l_x    = 110;
        s_eye_r_x    = 170;
        s_pupils_present = false;
        break;
    case EYE_BIG:
        // 40×40 outer with a 12×12 pupil rendered in the BG colour so it
        // reads as a hole. Outer slightly overshoots the original eye box
        // to look distinctly "bigger" without crowding the brow row.
        s_eye_open_w = 40;
        s_eye_open_h = 40;
        s_eye_open_y = 65 + FACE_Y_OFFSET;
        s_eye_l_x    = 110;
        s_eye_r_x    = 170;
        s_pupils_present = true;
        break;
    }
    // Reset to the open state regardless of where the blink machine left us.
    blink_paint(0);
    if (s_pupil_l) {
        if (s_pupils_present) {
            // Pupil 12×12 centred in the 40×40 outer.
            lv_obj_set_size(s_pupil_l, 12, 12);
            lv_obj_set_pos (s_pupil_l, s_eye_l_x + (s_eye_open_w - 12) / 2,
                                       s_eye_open_y + (s_eye_open_h - 12) / 2);
            lv_obj_remove_flag(s_pupil_l, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pupil_l, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_pupil_r) {
        if (s_pupils_present) {
            lv_obj_set_size(s_pupil_r, 12, 12);
            lv_obj_set_pos (s_pupil_r, s_eye_r_x + (s_eye_open_w - 12) / 2,
                                       s_eye_open_y + (s_eye_open_h - 12) / 2);
            lv_obj_remove_flag(s_pupil_r, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pupil_r, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void apply_mouth_variant(mouth_variant_t v) {
    // While talking, the static mouth is hidden anyway and the talking
    // sprite owns the display. We still apply visibility flags so the
    // right blocks come back when set_talking(false) flips s_smile back on.
    //
    // s_smile layout (matches k_smile order):
    //   [0] left tip,  [1] right tip
    //   [2] left shoulder, [3] right shoulder
    //   [4..13] base arc (10 blocks across the bottom, X 110..200)
    //
    // SMILE: every block visible — the original Daemon arc.
    // SMIRK: hide right tip + right shoulder so the mouth rises only on
    //        the left side. Asymmetric "knowing" curve.
    // FLAT:  hide every tip + shoulder + the outer 4 base blocks. Only
    //        the middle 6 base blocks (X 130..180) stay — reads as a
    //        clean horizontal line, the "_" of "- _ -".
    bool show[14];
    switch (v) {
    case MOUTH_SMILE:
    default:
        for (int i = 0; i < 14; i++) show[i] = true;
        break;
    case MOUTH_SMIRK:
        show[0] = true;   // left tip
        show[1] = false;  // right tip — hidden for asymmetry
        show[2] = true;   // left shoulder
        show[3] = false;  // right shoulder — hidden
        for (int i = 4; i < 14; i++) show[i] = true;  // full base arc
        break;
    case MOUTH_FLAT:
        show[0] = show[1] = show[2] = show[3] = false;
        // Base arc indices 6..11 are the inner 6 blocks (X 130..180).
        for (int i = 4; i < 14; i++) show[i] = (i >= 6 && i <= 11);
        break;
    }
    for (int i = 0; i < 14; i++) {
        if (!s_smile[i]) continue;
        if (show[i]) lv_obj_remove_flag(s_smile[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag   (s_smile[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_brow_variant(brow_variant_t v) {
    bool visible = (v != BROW_NONE);
    const int16_t *yl = NULL;
    const int16_t *yr = NULL;
    switch (v) {
    case BROW_ANGRY:  yl = k_brow_y_angry_l;  yr = k_brow_y_angry_r;  break;
    case BROW_RAISED: yl = k_brow_y_raised_l; yr = k_brow_y_raised_r; break;
    case BROW_NONE:
    default: break;
    }
    for (int i = 0; i < BROW_BLOCKS_PER_SIDE; i++) {
        if (s_brow_l[i]) {
            if (visible) {
                lv_obj_set_pos(s_brow_l[i], k_brow_x_l[i], yl[i] + FACE_Y_OFFSET);
                lv_obj_remove_flag(s_brow_l[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_brow_l[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_brow_r[i]) {
            if (visible) {
                lv_obj_set_pos(s_brow_r[i], k_brow_x_r[i], yr[i] + FACE_Y_OFFSET);
                lv_obj_remove_flag(s_brow_r[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_brow_r[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void apply_creature_traits(int idx) {
    if (idx < 0 || idx >= CREATURE_COUNT) idx = 0;
    s_creature_idx = idx;
    apply_eye_variant  (CREATURES[idx].eye);
    apply_mouth_variant(CREATURES[idx].mouth);
    apply_brow_variant (CREATURES[idx].brow);
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

    // --- variant-only widgets: pupils, oval mouth, brows ------------------
    // All hidden by default — apply_creature_traits() at the end of init
    // will reveal whichever set the active creature wears.

    // Pupils: BG-coloured 12×12 squares that ride inside the EYE_BIG outer
    // block to look like "hole in the eye". Created as siblings of the
    // eyes (rather than children) so resize/repositioning by the variant
    // appliers stays straightforward.
    s_pupil_l = make_block(s_scr, 0, 0, 12, 12, SCR_COLOR_BG);
    s_pupil_r = make_block(s_scr, 0, 0, 12, 12, SCR_COLOR_BG);
    lv_obj_add_flag(s_pupil_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pupil_r, LV_OBJ_FLAG_HIDDEN);

    // Brow blocks — 3 per side, positioned by apply_brow_variant().
    // Created at (0, 0) here; their real positions land when the active
    // variant is applied below.
    for (int i = 0; i < BROW_BLOCKS_PER_SIDE; i++) {
        s_brow_l[i] = make_block(s_scr, 0, 0, PX, PX, face);
        s_brow_r[i] = make_block(s_scr, 0, 0, PX, PX, face);
        lv_obj_add_flag(s_brow_l[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_brow_r[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Apply whichever creature was selected last boot. devcfg loaded the
    // index from NVS; default 0 = original Daemon, so first-boot behaviour
    // is unchanged.
    apply_creature_traits((int)devcfg_creature_index());

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

    // Push-to-talk lives on the BSP_BUTTON_MAIN hardware button (front-face,
    // below the LCD) — see buttons.c. The face itself stays free for tap
    // gestures we'll add later.

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
    // header — simpler to just stash in a static. See creature_screen_ptt_stop.
    extern volatile size_t s_speech_frames;
    frames = s_speech_frames;
    s_speech_frames = 0;

    // Filler word, ~50% of the time, fired in parallel with STT.
    // voice_speak() enqueues onto the audio task's queue and returns
    // immediately; the ElevenLabs HTTP fetch runs on the audio task while
    // we upload PCM to /v1/audio/transcriptions on this task. By the time
    // the user hears the filler (~700 ms after the dispatch when TLS is
    // cold, ~150 ms when warm), STT is already in flight, so the filler
    // genuinely covers what would otherwise be dead air. The actual reply
    // queues behind the filler and plays when ready.
    static const char *const FILLERS[] = {
        "Hmm.",
        "Let me see.",
        "Hmm, let me check.",
        "Alright.",
        "Okay, one sec.",
        "Give me a moment.",
        "Hmm, thinking.",
        "Let me think about that.",
    };
    if ((esp_random() & 1u) == 0u) {
        uint32_t idx = esp_random() % (sizeof(FILLERS) / sizeof(FILLERS[0]));
        voice_speak(FILLERS[idx]);
    }

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
    // Whisper heard before the AI reply replaces it.
    char displayed[600];
    snprintf(displayed, sizeof(displayed), "you: %s", transcript);
    ui_set_subtitle(displayed);
    ESP_LOGI(TAG, "transcript surfaced: %s", transcript);

    // Streaming path: chat.ts:chatStream now emits OpenAI-shaped tool_calls
    // SSE for every provider (openai/anthropic/grok), so tools work
    // uniformly across the matrix. First audio plays ~1.5–2 s sooner than
    // the buffered version because each sentence boundary triggers
    // voice_speak_chunk as the model emits it.
    //
    // Exception: shannon/* doesn't have a `/stream` route on the bundle
    // (Shannon's upstream API doesn't expose a streaming completion
    // endpoint, and we never wrote a server-side fake-SSE wrapper for
    // /api/call). Hitting /api/call/stream from the firmware returns 404.
    // For shannon models we fall back to the buffered ai_ask + a single
    // voice_speak — slower-feeling but functional.
    const char *model = devcfg_llm_model();
    bool is_shannon = model && strncasecmp(model, "shannon/", 8) == 0;

    char reply[1024] = {0};
    bool ok = is_shannon
        ? ai_ask(transcript, reply, sizeof(reply))
        : ai_ask_streaming(transcript, reply, sizeof(reply));
    if (ok && reply[0]) {
        ui_set_subtitle(reply);
        if (is_shannon) {
            // Buffered path doesn't auto-speak — synthesise once here.
            voice_speak(reply);
        }
        // Streaming path already played sentence-by-sentence; no voice_speak.
    } else if (reply[0]) {
        // The chat helper wrote a human-readable error; surface it instead
        // of the user's transcript.
        ui_set_subtitle(reply);
    }
    s_speech_in_flight = false;
    vTaskDelete(NULL);
}

volatile size_t s_speech_frames = 0;

void creature_screen_ptt_start(void) {
    if (s_speech_in_flight || mic_is_recording()) return;
    if (mic_record_start() == ESP_OK) {
        ui_set_subtitle("Listening...");
    } else {
        ESP_LOGW(TAG, "mic_record_start failed");
    }
}

void creature_screen_ptt_stop(void) {
    if (!mic_is_recording()) return;
    int16_t *pcm    = NULL;
    size_t   frames = 0;
    esp_err_t err = mic_record_stop(&pcm, &frames);
    if (err != ESP_OK || !pcm || frames < 1600) {   // <100 ms = junk
        if (pcm) mic_buffer_free(pcm);
        ui_set_subtitle("");
        return;
    }
    // Backchannel: a short tone confirms "I heard you" while STT + LLM
    // grind in the background. Cuts ~700 ms of perceived dead-air silence
    // before any audio comes back. No-op if voice subsystem is muted/down.
    voice_chirp();
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
    for (int i = 0; i < BROW_BLOCKS_PER_SIDE; i++) {
        if (s_brow_l[i]) lv_obj_set_style_bg_color(s_brow_l[i], col, LV_PART_MAIN);
        if (s_brow_r[i]) lv_obj_set_style_bg_color(s_brow_r[i], col, LV_PART_MAIN);
    }
    // Pupils intentionally NOT recoloured — they stay BG-coloured so the
    // EYE_BIG variant keeps its "hole" silhouette regardless of mood.
    lvgl_port_unlock();
}

void creature_screen_cycle(int delta) {
    if (!s_scr) return;
    int next = ((int)devcfg_creature_index() + delta) % CREATURE_COUNT;
    if (next < 0) next += CREATURE_COUNT;
    devcfg_set_creature_index((uint8_t)next);
    if (!lvgl_port_lock(0)) return;
    apply_creature_traits(next);
    // Re-apply mood so the freshly-shown variant widgets pick up the
    // current colour instead of the seed `face` colour from init().
    creature_mood_t m = s_mood;
    s_mood = (creature_mood_t)-1;
    lvgl_port_unlock();
    creature_screen_set_mood(m);
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
        // Re-apply the ACTIVE creature's mouth variant — not blanket
        // "show every smile block". Without this, creatures 1 (smirk) and
        // 2 (flat) snap back to a full smile every time the LLM finishes
        // talking, ignoring whichever variant the user is on.
        apply_mouth_variant(CREATURES[s_creature_idx].mouth);
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
