# Creature Designer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the variant-based creature face system with a uniform pixel-grid model, ship a browser-based pencil designer, and grow the roster to 6 creatures.

**Architecture:** Each creature becomes a set of bitmasks (one per face slot — brows, eyes, mouth) consumed by a single generic `slot_paint()` helper in firmware. Slot positions and sizes are fixed across all creatures. The HTML designer paints into the same standardized template and exports C code the user pastes into the repo.

**Tech Stack:** ESP-IDF (C, LVGL v9), single static HTML file (vanilla JS, no build step). Verification is `idf.py build` + on-device visual check.

**Spec:** `docs/superpowers/specs/2026-04-30-creature-designer-design.md`

---

## File Structure

**New:**
- `src/creatures_data.h` — declares `creature_data_t` struct + `CREATURES_DATA[6]` array.
- `src/creatures_data.c` — array literal with bitmasks for all 6 creatures.
- `tools/creature-designer.html` — static page, embedded CSS + vanilla JS.

**Modified:**
- `src/creature_screen.c` — major refactor (drop variant system, use pixel grids).
- `src/devcfg.c` — bump `CREATURE_COUNT` from 3 to 6.
- `src/CMakeLists.txt` — add `creatures_data.c` to `SRCS`.

**Untouched (despite being in spec):**
- `src/creature_screen.h` — public API stays the same.

---

## Verification Approach

This codebase has no host-side unit test framework — only on-device E2E tests via USB (`tests/run.py`). Verification per task is one or more of:

- **Compile clean:** `cd /Users/yordanlasonov/Documents/GitHub/board-game && idf.py build` (or via PlatformIO: `pio run`)
- **On-device visual:** flash + cycle through creatures, confirm look matches expectation.
- **HTML smoke:** open the page in a browser, perform the action, observe live preview reacts correctly.

Each task lists which verification(s) apply.

---

# Phase 1 — Firmware refactor

## Task 1: Add `creatures_data.h` + `creatures_data.c`

**Files:**
- Create: `src/creatures_data.h`
- Create: `src/creatures_data.c`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create the header**

```c
// src/creatures_data.h
// ---------------------------------------------------------------------------
//  Creature face bitmasks. Generated source: tools/creature-designer.html.
//  Hand-edits are tolerated (the designer reads this file's array literals
//  back), but the path of least surprise is: design in the tool, export, paste.
//
//  Slot grid layout (all positions raw, before FACE_Y_OFFSET):
//    brow_l : 2 rows × 4 cols, anchored at (110, 40), cells 10×10 px
//    brow_r : 2 rows × 4 cols, anchored at (170, 40)
//    eye_l  : 4 rows × 4 cols, anchored at (110, 60)
//    eye_r  : 4 rows × 4 cols, anchored at (170, 60)
//    mouth  : 3 rows × 14 cols, anchored at (90, 140)
//
//  The talking sprite (6×3 frames) is shared across creatures and lives in
//  creature_screen.c, not here.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

#define CREATURE_DATA_COUNT  6

typedef struct {
    const char *name;
    uint8_t brow_l[2][4];
    uint8_t brow_r[2][4];
    uint8_t eye_l [4][4];
    uint8_t eye_r [4][4];
    uint8_t mouth [3][14];
} creature_data_t;

extern const creature_data_t CREATURES_DATA[CREATURE_DATA_COUNT];
```

- [ ] **Step 2: Create the array literal**

The first three entries are bitmask ports of the existing variants (round/smile/none, slit/smirk/angry, big/flat/raised). The remaining three are copies of creature 0 — placeholders the user replaces in the designer. Coordinates and the derivation are in the design spec.

```c
// src/creatures_data.c
// ---------------------------------------------------------------------------
//  Creature roster. Edit via tools/creature-designer.html and paste the
//  exported array literal below. Hand-editing is fine for surgical tweaks
//  (e.g. names) but bulk pixel changes are easier in the tool.
// ---------------------------------------------------------------------------
#include "creatures_data.h"

const creature_data_t CREATURES_DATA[CREATURE_DATA_COUNT] = {
    // 0 — Daemon (round eyes, full smile, no brows)
    {
        .name = "Daemon",
        .brow_l = { {0,0,0,0}, {0,0,0,0} },
        .brow_r = { {0,0,0,0}, {0,0,0,0} },
        .eye_l  = { {0,1,1,0}, {1,1,1,1}, {1,1,1,1}, {0,1,1,0} },
        .eye_r  = { {0,1,1,0}, {1,1,1,1}, {1,1,1,1}, {0,1,1,0} },
        .mouth  = {
            {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
            {0,1,0,0,0,0,0,0,0,0,0,0,1,0},
            {0,0,1,1,1,1,1,1,1,1,1,1,0,0},
        },
    },
    // 1 — Laser (slit eyes, smirk, angry brows)
    {
        .name = "Laser",
        .brow_l = { {1,0,0,0}, {0,1,1,0} },
        .brow_r = { {0,0,1,0}, {1,1,0,0} },
        .eye_l  = { {0,0,0,0}, {0,0,0,0}, {1,1,1,1}, {0,0,0,0} },
        .eye_r  = { {0,0,0,0}, {0,0,0,0}, {1,1,1,1}, {0,0,0,0} },
        .mouth  = {
            {1,0,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,1,1,1,1,1,1,1,1,0,0},
        },
    },
    // 2 — Owl (big eyes with hollow pupils, flat mouth, raised brows)
    {
        .name = "Owl",
        .brow_l = { {0,1,0,0}, {1,0,1,0} },
        .brow_r = { {0,1,0,0}, {1,0,1,0} },
        .eye_l  = { {1,1,1,1}, {1,0,0,1}, {1,0,0,1}, {1,1,1,1} },
        .eye_r  = { {1,1,1,1}, {1,0,0,1}, {1,0,0,1}, {1,1,1,1} },
        .mouth  = {
            {0,0,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,0,0,1,1,1,1,1,1,0,0,0,0},
        },
    },
    // 3 — placeholder, replace via designer
    {
        .name = "Slot 3",
        .brow_l = { {0,0,0,0}, {0,0,0,0} },
        .brow_r = { {0,0,0,0}, {0,0,0,0} },
        .eye_l  = { {0,1,1,0}, {1,1,1,1}, {1,1,1,1}, {0,1,1,0} },
        .eye_r  = { {0,1,1,0}, {1,1,1,1}, {1,1,1,1}, {0,1,1,0} },
        .mouth  = {
            {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
            {0,1,0,0,0,0,0,0,0,0,0,0,1,0},
            {0,0,1,1,1,1,1,1,1,1,1,1,0,0},
        },
    },
    // 4 — placeholder
    {
        .name = "Slot 4",
        .brow_l = { {0,0,0,0}, {0,0,0,0} },
        .brow_r = { {0,0,0,0}, {0,0,0,0} },
        .eye_l  = { {0,1,1,0}, {1,1,1,1}, {1,1,1,1}, {0,1,1,0} },
        .eye_r  = { {0,1,1,0}, {1,1,1,1}, {1,1,1,1}, {0,1,1,0} },
        .mouth  = {
            {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
            {0,1,0,0,0,0,0,0,0,0,0,0,1,0},
            {0,0,1,1,1,1,1,1,1,1,1,1,0,0},
        },
    },
    // 5 — placeholder
    {
        .name = "Slot 5",
        .brow_l = { {0,0,0,0}, {0,0,0,0} },
        .brow_r = { {0,0,0,0}, {0,0,0,0} },
        .eye_l  = { {0,1,1,0}, {1,1,1,1}, {1,1,1,1}, {0,1,1,0} },
        .eye_r  = { {0,1,1,0}, {1,1,1,1}, {1,1,1,1}, {0,1,1,0} },
        .mouth  = {
            {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
            {0,1,0,0,0,0,0,0,0,0,0,0,1,0},
            {0,0,1,1,1,1,1,1,1,1,1,1,0,0},
        },
    },
};
```

- [ ] **Step 3: Wire into CMakeLists**

In `src/CMakeLists.txt`, add `"creatures_data.c"` to the `SRCS` list directly under `"creature_screen.c"`. The existing SRCS list is:

```cmake
    SRCS
        "ai.c"
        "app_main.c"
        ...
        "creature_screen.c"
        "devcfg.c"
        ...
```

Becomes:

```cmake
    SRCS
        "ai.c"
        "app_main.c"
        ...
        "creature_screen.c"
        "creatures_data.c"
        "devcfg.c"
        ...
```

- [ ] **Step 4: Verify it compiles**

Run: `cd /Users/yordanlasonov/Documents/GitHub/board-game && pio run`

Expected: builds clean. The new file isn't referenced anywhere yet, so it should just sit there compiled but unused.

If the build fails complaining about an unused variable, that's fine — it shouldn't, because we declared `CREATURES_DATA` with `extern`. If you see a warning about no consumer, ignore it.

- [ ] **Step 5: Commit**

```bash
git add src/creatures_data.h src/creatures_data.c src/CMakeLists.txt
git commit -m "creatures: add bitmask data table (no consumer yet)"
```

---

## Task 2: Refactor `creature_screen.c` — drop variant system, add pixel grids

This is the big atomic change. It replaces the variant enums and per-variant geometry tables with the uniform pixel-grid model. Every face slot becomes a 2D array of 10×10 widgets pre-created at init; the active creature's bitmask decides which cells are visible.

**Files:**
- Modify: `src/creature_screen.c` (significant — see steps below)

The header `src/creature_screen.h` is unchanged.

- [ ] **Step 1: Replace the static-state block (top of file, lines ~50-200)**

Open `src/creature_screen.c`. Replace the section from line ~50 (just after `#include` block) through line ~200 (end of variant trait declarations) with the new state block below. Specifically:

- KEEP: the include block, `static const char *TAG`, the `#define PX 10`, `FACE_Y_OFFSET`.
- REMOVE: `EYE_W`, `EYE_H`, `MOUTH_W`, `EYE_L_X`, `EYE_R_X`, `EYE_Y`, `SMILE_MID_Y` (`SMILE_MID_Y` will be re-introduced near the talking mouth), `s_eye_l`, `s_eye_r`, `s_smile[14]`, `k_smile`, `creature_traits_t`, `eye_variant_t`, `mouth_variant_t`, `brow_variant_t`, `CREATURES[]`, `s_creature_idx`, `s_eye_open_*`, `s_eye_l_x`, `s_eye_r_x`, `s_pupil_l`, `s_pupil_r`, `BROW_BLOCKS_PER_SIDE`, `s_brow_l[]`, `s_brow_r[]`, `s_pupils_present`, `k_brow_x_l`, `k_brow_x_r`, `k_brow_y_angry_*`, `k_brow_y_raised_*`.
- KEEP: `s_scr`, `s_price_label`, `s_usdc_label`, the talking mouth grid + `MOUTH_FRAMES` + `MOUTH_CYCLE` + `s_mouth_timer` + `s_mouth_cyc`, `s_subtitle` + pagination state + `s_sub_*`, blink frame structs (`BLINK_*`), `s_blink_timer`, `s_blink_seq`, `s_blink_step`, `s_mood`, `s_talking`.

Here is the new block to insert in the file's static-state section. Place it AFTER the talking-mouth state and BEFORE the blink state (it doesn't matter much, but this keeps related concerns adjacent):

```c
// --- pixel-grid slot state -------------------------------------------------
// All face slots are fixed-size grids of 10×10 widgets pre-created at init.
// A creature is just bitmasks (in CREATURES_DATA[]) saying which cells are
// visible. Same blink + talking machinery works for any creature.
//
// Slot positions are RAW (before FACE_Y_OFFSET). PX (=10) is the cell size.

#define BROW_ROWS  2
#define BROW_COLS  4
#define EYE_ROWS   4
#define EYE_COLS   4
#define MIDLE_ROWS 3   // mouth-idle rows
#define MIDLE_COLS 14  // mouth-idle cols

#define BROW_L_X   110
#define BROW_R_X   170
#define BROW_Y     40

#define EYE_L_X    110
#define EYE_R_X    170
#define EYE_Y      60

#define MIDLE_X    90
#define MIDLE_Y    140

// Closed-eye bar — 4 cells (40×10 px) along the bottom row of each eye area.
// Pre-created hidden; shown during a closed blink frame instead of resizing
// the eye widgets. Generic — works for any eye bitmask.
#define EYE_BAR_W  (EYE_COLS * PX)
#define EYE_BAR_H  PX

static lv_obj_t *s_brow_grid_l[BROW_ROWS][BROW_COLS]   = {{0}};
static lv_obj_t *s_brow_grid_r[BROW_ROWS][BROW_COLS]   = {{0}};
static lv_obj_t *s_eye_grid_l [EYE_ROWS ][EYE_COLS ]   = {{0}};
static lv_obj_t *s_eye_grid_r [EYE_ROWS ][EYE_COLS ]   = {{0}};
static lv_obj_t *s_mouth_idle [MIDLE_ROWS][MIDLE_COLS] = {{0}};
static lv_obj_t *s_eye_bar_l                           = NULL;
static lv_obj_t *s_eye_bar_r                           = NULL;

// Active creature index (0..CREATURE_DATA_COUNT-1). Initialized from NVS at
// init() and updated by creature_screen_cycle(). The current bitmasks are
// always &CREATURES_DATA[s_creature_idx].
static int s_creature_idx = 0;
```

If the talking-mouth section's `SMILE_MID_Y` constant got removed during the deletion sweep, reintroduce it with its existing value so the talking sprite stays at the same on-device coordinates:

```c
#define SMILE_MID_Y    (165 + FACE_Y_OFFSET)
```

The talking sprite's position is independent of the mouth-idle grid — they're separate regions that overlap visually but don't share state.

- [ ] **Step 2: Add the include**

At the top of `src/creature_screen.c`, near the other internal includes (`#include "ai.h"` etc), add:

```c
#include "creatures_data.h"
```

- [ ] **Step 3: Add `slot_paint()` helper**

Place this in the helpers section, near `make_block` (around line ~265 in the current file):

```c
// Apply a row-major bitmask to a row-major widget array. Cells with mask=0
// are hidden, cells with mask=1 are shown. Used uniformly for every face
// slot (brows / eyes / mouth-idle).
static void slot_paint(lv_obj_t * const *grid, const uint8_t *mask, int n) {
    for (int i = 0; i < n; i++) {
        if (!grid[i]) continue;
        if (mask[i]) lv_obj_remove_flag(grid[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag   (grid[i], LV_OBJ_FLAG_HIDDEN);
    }
}
```

The grid arrays passed are 2D (`lv_obj_t *[R][C]`), but C decays them to `lv_obj_t *[]` row-major when you cast through a pointer. Call sites use `(lv_obj_t * const *)&grid[0][0]` and `&mask[0][0]`.

- [ ] **Step 4: Replace `apply_creature_traits()` with `apply_creature()`**

Find `apply_creature_traits` (around line ~492) and the three `apply_*_variant` functions. Delete all four. Replace with:

```c
// Apply the active creature's bitmasks to all face slots. Caller holds the
// lvgl_port lock. Idempotent — safe to call repeatedly with the same idx.
static void apply_creature(int idx) {
    if (idx < 0 || idx >= CREATURE_DATA_COUNT) idx = 0;
    s_creature_idx = idx;
    const creature_data_t *c = &CREATURES_DATA[idx];

    slot_paint((lv_obj_t * const *)&s_brow_grid_l[0][0], &c->brow_l[0][0],
               BROW_ROWS * BROW_COLS);
    slot_paint((lv_obj_t * const *)&s_brow_grid_r[0][0], &c->brow_r[0][0],
               BROW_ROWS * BROW_COLS);
    slot_paint((lv_obj_t * const *)&s_eye_grid_l[0][0],  &c->eye_l[0][0],
               EYE_ROWS * EYE_COLS);
    slot_paint((lv_obj_t * const *)&s_eye_grid_r[0][0],  &c->eye_r[0][0],
               EYE_ROWS * EYE_COLS);
    // Only paint mouth-idle if not currently talking; otherwise the talking
    // sprite owns the mouth area and we'd flicker.
    if (!s_talking) {
        slot_paint((lv_obj_t * const *)&s_mouth_idle[0][0], &c->mouth[0][0],
                   MIDLE_ROWS * MIDLE_COLS);
    }
}
```

- [ ] **Step 5: Replace `blink_paint()`**

Find the existing `blink_paint()` (around line ~327). Delete it. Replace with:

```c
// Closed-eye renders as a 4-cell bottom bar (s_eye_bar_*); open-eye shows
// the pixel grid. Generic — works for any creature's eye pattern.
static void blink_paint(uint8_t eyes) {
    bool lc = (eyes & EYE_CLOSED_L) != 0;
    bool rc = (eyes & EYE_CLOSED_R) != 0;
    const creature_data_t *c = &CREATURES_DATA[s_creature_idx];

    // Left eye
    for (int r = 0; r < EYE_ROWS; r++) {
        for (int col = 0; col < EYE_COLS; col++) {
            if (!s_eye_grid_l[r][col]) continue;
            if (lc) {
                lv_obj_add_flag(s_eye_grid_l[r][col], LV_OBJ_FLAG_HIDDEN);
            } else if (c->eye_l[r][col]) {
                lv_obj_remove_flag(s_eye_grid_l[r][col], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_eye_grid_l[r][col], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (s_eye_bar_l) {
        if (lc) lv_obj_remove_flag(s_eye_bar_l, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag   (s_eye_bar_l, LV_OBJ_FLAG_HIDDEN);
    }

    // Right eye
    for (int r = 0; r < EYE_ROWS; r++) {
        for (int col = 0; col < EYE_COLS; col++) {
            if (!s_eye_grid_r[r][col]) continue;
            if (rc) {
                lv_obj_add_flag(s_eye_grid_r[r][col], LV_OBJ_FLAG_HIDDEN);
            } else if (c->eye_r[r][col]) {
                lv_obj_remove_flag(s_eye_grid_r[r][col], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_eye_grid_r[r][col], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (s_eye_bar_r) {
        if (rc) lv_obj_remove_flag(s_eye_bar_r, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag   (s_eye_bar_r, LV_OBJ_FLAG_HIDDEN);
    }
}
```

- [ ] **Step 6: Replace the widget-creation block in `creature_screen_init()`**

In `creature_screen_init()` (starts around line ~534), find the section that creates eye widgets, smile widgets, brow widgets, pupils. Replace it with:

```c
    lv_color_t face = SCR_COLOR_ACCENT;

    // --- brow grids: 2×4 cells per side, 10×10 cells, hidden by default ---
    for (int r = 0; r < BROW_ROWS; r++) {
        for (int col = 0; col < BROW_COLS; col++) {
            int x_l = BROW_L_X + col * PX;
            int x_r = BROW_R_X + col * PX;
            int y   = BROW_Y   + r   * PX + FACE_Y_OFFSET;
            s_brow_grid_l[r][col] = make_block(s_scr, x_l, y, PX, PX, face);
            s_brow_grid_r[r][col] = make_block(s_scr, x_r, y, PX, PX, face);
            lv_obj_add_flag(s_brow_grid_l[r][col], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_brow_grid_r[r][col], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // --- eye grids: 4×4 cells per side ------------------------------------
    for (int r = 0; r < EYE_ROWS; r++) {
        for (int col = 0; col < EYE_COLS; col++) {
            int x_l = EYE_L_X + col * PX;
            int x_r = EYE_R_X + col * PX;
            int y   = EYE_Y   + r   * PX + FACE_Y_OFFSET;
            s_eye_grid_l[r][col] = make_block(s_scr, x_l, y, PX, PX, face);
            s_eye_grid_r[r][col] = make_block(s_scr, x_r, y, PX, PX, face);
            lv_obj_add_flag(s_eye_grid_l[r][col], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_eye_grid_r[r][col], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // --- closed-eye bars: 40×10 along the bottom row of each eye area -----
    {
        int bar_y = EYE_Y + (EYE_ROWS - 1) * PX + FACE_Y_OFFSET;
        s_eye_bar_l = make_block(s_scr, EYE_L_X, bar_y, EYE_BAR_W, EYE_BAR_H, face);
        s_eye_bar_r = make_block(s_scr, EYE_R_X, bar_y, EYE_BAR_W, EYE_BAR_H, face);
        lv_obj_add_flag(s_eye_bar_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_eye_bar_r, LV_OBJ_FLAG_HIDDEN);
    }

    // --- mouth-idle grid: 3×14 cells, hidden by default -------------------
    for (int r = 0; r < MIDLE_ROWS; r++) {
        for (int col = 0; col < MIDLE_COLS; col++) {
            int x = MIDLE_X + col * PX;
            int y = MIDLE_Y + r   * PX + FACE_Y_OFFSET;
            s_mouth_idle[r][col] = make_block(s_scr, x, y, PX, PX, face);
            lv_obj_add_flag(s_mouth_idle[r][col], LV_OBJ_FLAG_HIDDEN);
        }
    }
```

The talking-mouth grid (`s_mouth_grid`) creation block stays exactly as it is — that grid is unchanged.

- [ ] **Step 7: Replace the `apply_creature_traits` call near the end of `creature_screen_init()`**

Find the line:
```c
apply_creature_traits((int)devcfg_creature_index());
```

Replace with:
```c
apply_creature((int)devcfg_creature_index());
```

- [ ] **Step 8: Update `creature_screen_cycle()`**

Find `creature_screen_cycle()` (around line ~957). Replace its body so it uses `CREATURE_DATA_COUNT` and `apply_creature()`:

```c
void creature_screen_cycle(int delta) {
    if (!s_scr) return;
    int next = ((int)devcfg_creature_index() + delta) % CREATURE_DATA_COUNT;
    if (next < 0) next += CREATURE_DATA_COUNT;
    devcfg_set_creature_index((uint8_t)next);
    if (!lvgl_port_lock(0)) return;
    apply_creature(next);
    // Re-apply mood so the freshly-shown variant widgets pick up the
    // current colour instead of the seed `face` colour from init().
    creature_mood_t m = s_mood;
    s_mood = (creature_mood_t)-1;
    lvgl_port_unlock();
    creature_screen_set_mood(m);
}
```

- [ ] **Step 9: Update `creature_screen_set_talking()`**

Find `creature_screen_set_talking()` (around line ~972). It currently calls `apply_mouth_variant(CREATURES[s_creature_idx].mouth)` on talking-off. Replace that call. Also, on talking-on, hide the mouth-idle grid:

```c
void creature_screen_set_talking(bool on) {
    if (s_talking == on) return;
    s_talking = on;

    if (!lvgl_port_lock(0)) return;
    if (on) {
        // Hide the mouth-idle grid; the talking sprite owns the mouth area.
        for (int r = 0; r < MIDLE_ROWS; r++) {
            for (int col = 0; col < MIDLE_COLS; col++) {
                if (s_mouth_idle[r][col])
                    lv_obj_add_flag(s_mouth_idle[r][col], LV_OBJ_FLAG_HIDDEN);
            }
        }
        s_mouth_cyc = 0;
        mouth_paint_frame(MOUTH_CYCLE[0]);
        if (s_mouth_timer) lv_timer_resume(s_mouth_timer);
    } else {
        if (s_mouth_timer) lv_timer_pause(s_mouth_timer);
        mouth_hide_all();
        // Re-apply the active creature's mouth bitmask. Without this, the
        // mouth-idle grid stays empty after the LLM finishes talking.
        const creature_data_t *c = &CREATURES_DATA[s_creature_idx];
        slot_paint((lv_obj_t * const *)&s_mouth_idle[0][0], &c->mouth[0][0],
                   MIDLE_ROWS * MIDLE_COLS);
    }
    lvgl_port_unlock();
}
```

- [ ] **Step 10: Update `creature_screen_set_mood()` to recolor the new widgets**

Find `creature_screen_set_mood()` (around line ~928). The body currently iterates `s_eye_l`, `s_eye_r`, `s_smile[]`, `s_mouth_grid[][]`, `s_brow_l[]`, `s_brow_r[]`, and skips pupils. Replace with:

```c
void creature_screen_set_mood(creature_mood_t m) {
    if (!s_scr) return;
    if (m == s_mood) return;
    s_mood = m;

    lv_color_t col = mood_color(m);

    if (!lvgl_port_lock(0)) return;

    // Recolor every face widget so the whole creature reads the same mood.
    for (int r = 0; r < BROW_ROWS; r++) {
        for (int c = 0; c < BROW_COLS; c++) {
            if (s_brow_grid_l[r][c]) lv_obj_set_style_bg_color(s_brow_grid_l[r][c], col, LV_PART_MAIN);
            if (s_brow_grid_r[r][c]) lv_obj_set_style_bg_color(s_brow_grid_r[r][c], col, LV_PART_MAIN);
        }
    }
    for (int r = 0; r < EYE_ROWS; r++) {
        for (int c = 0; c < EYE_COLS; c++) {
            if (s_eye_grid_l[r][c]) lv_obj_set_style_bg_color(s_eye_grid_l[r][c], col, LV_PART_MAIN);
            if (s_eye_grid_r[r][c]) lv_obj_set_style_bg_color(s_eye_grid_r[r][c], col, LV_PART_MAIN);
        }
    }
    if (s_eye_bar_l) lv_obj_set_style_bg_color(s_eye_bar_l, col, LV_PART_MAIN);
    if (s_eye_bar_r) lv_obj_set_style_bg_color(s_eye_bar_r, col, LV_PART_MAIN);
    for (int r = 0; r < MIDLE_ROWS; r++) {
        for (int c = 0; c < MIDLE_COLS; c++) {
            if (s_mouth_idle[r][c]) lv_obj_set_style_bg_color(s_mouth_idle[r][c], col, LV_PART_MAIN);
        }
    }
    for (int r = 0; r < MOUTH_ROWS; r++) {
        for (int c = 0; c < MOUTH_COLS; c++) {
            if (s_mouth_grid[r][c]) lv_obj_set_style_bg_color(s_mouth_grid[r][c], col, LV_PART_MAIN);
        }
    }

    lvgl_port_unlock();
}
```

- [ ] **Step 11: Update `set_smile_visible()` if still referenced; otherwise delete**

Search the file for `set_smile_visible`. If still referenced anywhere (it shouldn't be — it was used by the old `set_talking`), delete the function. If still referenced, replace the body to operate on `s_mouth_idle` instead of `s_smile`. (After step 9 above, no caller should remain, so just delete it.)

- [ ] **Step 12: Verify it compiles**

Run: `cd /Users/yordanlasonov/Documents/GitHub/board-game && pio run`

Expected: builds clean. Common errors and fixes:
- "undeclared identifier `s_smile`" — left a reference to the old smile array; search and remove.
- "unused function `set_smile_visible`" — delete the function.
- "unused variable `s_pupils_present`" — delete the line.

- [ ] **Step 13: Commit**

```bash
git add src/creature_screen.c
git commit -m "creature_screen: switch to pixel-grid bitmask model"
```

---

## Task 3: Bump `CREATURE_COUNT` to 6

**Files:**
- Modify: `src/devcfg.c`

- [ ] **Step 1: Update the constant**

In `src/devcfg.c`, find:
```c
#define CREATURE_COUNT      3
```

Change to:
```c
#define CREATURE_COUNT      6
```

The bound checks on lines 256 and 416 already use `CREATURE_COUNT`, so they pick up the new max automatically.

- [ ] **Step 2: Verify it compiles**

Run: `cd /Users/yordanlasonov/Documents/GitHub/board-game && pio run`

Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add src/devcfg.c
git commit -m "devcfg: lift creature count to 6"
```

---

## Task 4: Flash + on-device visual verification

**Files:** none (verification only)

- [ ] **Step 1: Flash to device**

```bash
cd /Users/yordanlasonov/Documents/GitHub/board-game && pio run -t upload
```

- [ ] **Step 2: Observe creature 0 (Daemon)**

On boot, the creature screen should show the original Daemon: round eyes (4×4 outline), full smile arc, no brows. Identical to what was on-device before this change.

- [ ] **Step 3: Cycle to creature 1 (Laser)**

Slide right (or use whichever input is bound to `creature_screen_cycle(+1)`). Expected: thin horizontal slit eyes (single row), asymmetric smirk (left tip + base arc, no right tip), angry slanted brows.

- [ ] **Step 4: Cycle to creature 2 (Owl)**

Expected: big rectangular eyes with hollow center (pupil-as-hole), flat horizontal mouth (middle 6 cells of base row), raised arch brows.

- [ ] **Step 5: Cycle through 3, 4, 5**

All three should look like creature 0 (placeholder copies). Cycling wraps back to 0 from 5.

- [ ] **Step 6: Wait for a blink on each creature**

Blinks occur every 2.5–6.5s. Verify the closed-eye renders as a thin horizontal bar at the bottom of the eye area for each creature, not as a collapsed widget.

- [ ] **Step 7: Trigger a talking sequence (PTT button)**

Press the front button, say something, release. While the LLM reply plays, the mouth-idle grid hides and the 6×3 talking sprite cycles. When TTS finishes, the mouth-idle grid reappears with the current creature's bitmask. Verify this works on at least creatures 0, 1, and 2.

- [ ] **Step 8: Commit verification artifacts (none needed)**

Nothing to commit — this task is verification only.

If anything looks wrong, fix it in the appropriate task (1, 2, or 3) and re-flash before continuing.

---

# Phase 2 — HTML designer

## Task 5: HTML scaffold with slot template

**Files:**
- Create: `tools/creature-designer.html`

The full file is large (~700 lines). We build it across this and the next several tasks. Each task adds one logical chunk and verifies it works in a browser before moving on.

- [ ] **Step 1: Create the file with the static scaffold**

```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Creature Designer</title>
<style>
  body {
    margin: 0;
    background: #111;
    color: #0f0;
    font-family: ui-monospace, "SF Mono", Menlo, monospace;
    font-size: 13px;
  }
  #app {
    display: flex;
    height: 100vh;
  }
  #editor {
    flex: 1 1 auto;
    padding: 16px;
    overflow: auto;
  }
  #right {
    flex: 0 0 360px;
    border-left: 1px solid #333;
    padding: 16px;
  }
  .toolbar {
    margin-bottom: 12px;
  }
  .toolbar button, .toolbar select, .toolbar input {
    background: #222;
    color: #0f0;
    border: 1px solid #333;
    padding: 4px 8px;
    margin-right: 4px;
    font: inherit;
  }
  .toolbar button.active {
    background: #0f0;
    color: #000;
  }
  .slot {
    display: inline-block;
    margin: 8px;
    vertical-align: top;
  }
  .slot h3 {
    margin: 0 0 4px;
    font-size: 11px;
    color: #888;
  }
  .grid {
    display: grid;
    gap: 0;
    border: 1px solid #333;
    background: #000;
  }
  .cell {
    width: 28px;
    height: 28px;
    border: 1px solid #1a1a1a;
    box-sizing: border-box;
    background: #000;
    cursor: crosshair;
  }
  .cell.on {
    background: #0f0;
  }
  #preview-wrap {
    border: 1px solid #333;
    width: 320px;
    height: 240px;
    position: relative;
    background: #000;
  }
  #preview-wrap .pixel {
    position: absolute;
    background: #0f0;
  }
</style>
</head>
<body>
<div id="app">
  <div id="editor">
    <div class="toolbar">
      <button id="t-pencil" class="active">Pencil</button>
      <button id="t-eraser">Eraser</button>
      <button id="t-mirror">Mirror: off</button>
      <span style="margin-left:16px">Creature: </span>
      <input id="creature-name" type="text" value="Daemon" style="width:120px">
      <button id="prev-creature">◀</button>
      <span id="creature-idx">1 / 6</span>
      <button id="next-creature">▶</button>
      <span style="margin-left:16px">
        <button id="btn-new">New</button>
        <button id="btn-load">Load</button>
        <button id="btn-save">Save</button>
        <button id="btn-export">Export C</button>
      </span>
    </div>

    <!-- Slot rows: brows on top of eyes, mouth below -->
    <div>
      <div class="slot">
        <h3>brow_l (2 × 4)</h3>
        <div class="grid" id="grid-brow_l" data-rows="2" data-cols="4"></div>
      </div>
      <div class="slot">
        <h3>brow_r (2 × 4)</h3>
        <div class="grid" id="grid-brow_r" data-rows="2" data-cols="4"></div>
      </div>
    </div>
    <div>
      <div class="slot">
        <h3>eye_l (4 × 4)</h3>
        <div class="grid" id="grid-eye_l" data-rows="4" data-cols="4"></div>
      </div>
      <div class="slot">
        <h3>eye_r (4 × 4)</h3>
        <div class="grid" id="grid-eye_r" data-rows="4" data-cols="4"></div>
      </div>
    </div>
    <div>
      <div class="slot">
        <h3>mouth (3 × 14)</h3>
        <div class="grid" id="grid-mouth" data-rows="3" data-cols="14"></div>
      </div>
    </div>
  </div>

  <div id="right">
    <h3 style="margin:0 0 8px">Live preview</h3>
    <div id="preview-wrap"></div>
    <div style="margin-top:12px">
      <label>Mood:
        <select id="mood">
          <option value="idle">idle</option>
          <option value="listen">listen</option>
          <option value="think">think</option>
          <option value="talk">talk</option>
          <option value="happy">happy</option>
          <option value="angry">angry</option>
        </select>
      </label>
      <label style="margin-left:12px">
        <input type="checkbox" id="talking"> Talking
      </label>
    </div>
  </div>
</div>

<script>
"use strict";

// ---------------------------------------------------------------------------
//  Data model
// ---------------------------------------------------------------------------
const SLOTS = [
  { key: "brow_l", rows: 2, cols: 4 },
  { key: "brow_r", rows: 2, cols: 4 },
  { key: "eye_l",  rows: 4, cols: 4 },
  { key: "eye_r",  rows: 4, cols: 4 },
  { key: "mouth",  rows: 3, cols: 14 },
];

function blankCreature(name) {
  const c = { name };
  for (const s of SLOTS) {
    c[s.key] = Array.from({length: s.rows}, () => Array(s.cols).fill(0));
  }
  return c;
}

const roster = Array.from({length: 6}, (_, i) => blankCreature(`Slot ${i+1}`));
let activeIdx = 0;

// ---------------------------------------------------------------------------
//  Build the editor grids
// ---------------------------------------------------------------------------
function buildGrid(slot) {
  const el = document.getElementById(`grid-${slot.key}`);
  el.style.gridTemplateColumns = `repeat(${slot.cols}, 28px)`;
  el.style.gridTemplateRows    = `repeat(${slot.rows}, 28px)`;
  el.innerHTML = "";
  for (let r = 0; r < slot.rows; r++) {
    for (let c = 0; c < slot.cols; c++) {
      const cell = document.createElement("div");
      cell.className = "cell";
      cell.dataset.slot = slot.key;
      cell.dataset.r = r;
      cell.dataset.c = c;
      el.appendChild(cell);
    }
  }
}
SLOTS.forEach(buildGrid);

// ---------------------------------------------------------------------------
//  Render
// ---------------------------------------------------------------------------
function refreshEditor() {
  const c = roster[activeIdx];
  for (const slot of SLOTS) {
    const cells = document.querySelectorAll(`#grid-${slot.key} .cell`);
    cells.forEach(el => {
      const r = +el.dataset.r;
      const col = +el.dataset.c;
      el.classList.toggle("on", !!c[slot.key][r][col]);
    });
  }
  document.getElementById("creature-name").value = c.name;
  document.getElementById("creature-idx").textContent = `${activeIdx+1} / 6`;
}
refreshEditor();
</script>
</body>
</html>
```

- [ ] **Step 2: Open in a browser**

Run: `open /Users/yordanlasonov/Documents/GitHub/board-game/tools/creature-designer.html`

Expected: dark page with five empty grid templates (2×4 brow-l, 2×4 brow-r, 4×4 eye-l, 4×4 eye-r, 3×14 mouth), a toolbar at the top, a placeholder preview panel on the right. Cells are dark squares; no cells are green yet.

- [ ] **Step 3: Commit**

```bash
git add tools/creature-designer.html
git commit -m "designer: scaffold with slot grids and toolbar"
```

---

## Task 6: Click-to-paint pencil and eraser

**Files:**
- Modify: `tools/creature-designer.html`

- [ ] **Step 1: Add tool state and click handlers**

Inside the `<script>` block, AFTER the `refreshEditor();` call, append:

```javascript
// ---------------------------------------------------------------------------
//  Painting
// ---------------------------------------------------------------------------
let tool = "pencil";   // "pencil" | "eraser"
let mirror = false;
let painting = false;
let paintValue = 1;    // value being applied during a drag

function setTool(t) {
  tool = t;
  document.getElementById("t-pencil").classList.toggle("active", t === "pencil");
  document.getElementById("t-eraser").classList.toggle("active", t === "eraser");
}
document.getElementById("t-pencil").addEventListener("click", () => setTool("pencil"));
document.getElementById("t-eraser").addEventListener("click", () => setTool("eraser"));

document.getElementById("t-mirror").addEventListener("click", () => {
  mirror = !mirror;
  document.getElementById("t-mirror").textContent = `Mirror: ${mirror ? "on" : "off"}`;
});

function mirrorOf(slotKey) {
  if (slotKey === "brow_l") return "brow_r";
  if (slotKey === "brow_r") return "brow_l";
  if (slotKey === "eye_l")  return "eye_r";
  if (slotKey === "eye_r")  return "eye_l";
  return null;
}

function paintCell(el, value) {
  const slotKey = el.dataset.slot;
  const r = +el.dataset.r;
  const c = +el.dataset.c;
  const cur = roster[activeIdx];
  cur[slotKey][r][c] = value;
  el.classList.toggle("on", !!value);

  if (mirror) {
    const m = mirrorOf(slotKey);
    if (m) {
      // Mirror by column: col c on left → col (cols-1-c) on right.
      const slot = SLOTS.find(s => s.key === m);
      const mc = slot.cols - 1 - c;
      cur[m][r][mc] = value;
      const mEl = document.querySelector(`#grid-${m} .cell[data-r="${r}"][data-c="${mc}"]`);
      if (mEl) mEl.classList.toggle("on", !!value);
    }
  }
  // Hook for live preview — added in a later task.
  if (typeof refreshPreview === "function") refreshPreview();
}

document.querySelectorAll(".grid").forEach(grid => {
  grid.addEventListener("mousedown", e => {
    if (!e.target.classList.contains("cell")) return;
    painting = true;
    paintValue = (tool === "pencil") ? 1 : 0;
    paintCell(e.target, paintValue);
  });
  grid.addEventListener("mouseover", e => {
    if (!painting) return;
    if (!e.target.classList.contains("cell")) return;
    paintCell(e.target, paintValue);
  });
});
document.addEventListener("mouseup", () => { painting = false; });
```

- [ ] **Step 2: Reload the browser and test**

Refresh the open `creature-designer.html` page.

Expected:
- Click a cell — it turns green.
- Click again with eraser selected — it turns black.
- Click + drag — multiple cells fill in a stroke.
- Toggle mirror; paint a left-eye cell — the corresponding right-eye cell mirrors automatically.

- [ ] **Step 3: Commit**

```bash
git add tools/creature-designer.html
git commit -m "designer: pencil + eraser + mirror painting"
```

---

## Task 7: Live preview at device resolution

**Files:**
- Modify: `tools/creature-designer.html`

The live preview shows the creature at the actual on-device pixel coordinates (320×240 panel) so the user sees exactly what will appear after flashing. We render absolute-positioned 10×10 divs inside the 320×240 wrapper, mirroring the firmware's coordinate system.

- [ ] **Step 1: Add the preview render function**

Append to the `<script>` block, after the painting code:

```javascript
// ---------------------------------------------------------------------------
//  Live preview — same coordinate system as firmware (320×240, 10px cells,
//  raw positions before FACE_Y_OFFSET, then offset applied at render time).
// ---------------------------------------------------------------------------
const PX = 10;
const FACE_Y_OFFSET = -5;
const PREVIEW_SLOTS = {
  brow_l: { x: 110, y: 40, rows: 2, cols: 4 },
  brow_r: { x: 170, y: 40, rows: 2, cols: 4 },
  eye_l:  { x: 110, y: 60, rows: 4, cols: 4 },
  eye_r:  { x: 170, y: 60, rows: 4, cols: 4 },
  mouth:  { x: 90,  y: 140, rows: 3, cols: 14 },
};

const MOOD_COLORS = {
  idle:   "#3a3a8c",  // accent
  listen: "#5a5acc",  // accent-hi
  think:  "#222",     // dim
  talk:   "#5a5acc",
  happy:  "#0c0",
  angry:  "#c80",
};
// In the firmware, SCR_COLOR_ACCENT is the palette green; we use a CRT green
// here so the designer reads as a faithful preview. Mood colors are tuned to
// approximate the firmware palette without being pixel-exact.

const previewWrap = document.getElementById("preview-wrap");

function refreshPreview() {
  const c = roster[activeIdx];
  const mood = document.getElementById("mood").value;
  const isTalking = document.getElementById("talking").checked;
  const color = MOOD_COLORS[mood] || "#0f0";

  previewWrap.innerHTML = "";

  // Brows + eyes (always rendered)
  for (const key of ["brow_l", "brow_r", "eye_l", "eye_r"]) {
    const slot = PREVIEW_SLOTS[key];
    for (let r = 0; r < slot.rows; r++) {
      for (let col = 0; col < slot.cols; col++) {
        if (!c[key][r][col]) continue;
        const div = document.createElement("div");
        div.className = "pixel";
        div.style.left   = (slot.x + col * PX) + "px";
        div.style.top    = (slot.y + r * PX + FACE_Y_OFFSET) + "px";
        div.style.width  = PX + "px";
        div.style.height = PX + "px";
        div.style.background = color;
        previewWrap.appendChild(div);
      }
    }
  }

  // Mouth: when talking, hidden (talking sprite renders separately in a
  // later task). When idle, render the bitmask.
  if (!isTalking) {
    const slot = PREVIEW_SLOTS.mouth;
    for (let r = 0; r < slot.rows; r++) {
      for (let col = 0; col < slot.cols; col++) {
        if (!c.mouth[r][col]) continue;
        const div = document.createElement("div");
        div.className = "pixel";
        div.style.left   = (slot.x + col * PX) + "px";
        div.style.top    = (slot.y + r * PX + FACE_Y_OFFSET) + "px";
        div.style.width  = PX + "px";
        div.style.height = PX + "px";
        div.style.background = color;
        previewWrap.appendChild(div);
      }
    }
  }
}

document.getElementById("mood").addEventListener("change", refreshPreview);
document.getElementById("talking").addEventListener("change", refreshPreview);
refreshPreview();
```

- [ ] **Step 2: Reload and test**

Refresh the page. Paint cells in the editor — preview pixels appear at the correct on-device positions. Change the mood selector — colors update.

- [ ] **Step 3: Commit**

```bash
git add tools/creature-designer.html
git commit -m "designer: live preview at device resolution"
```

---

## Task 8: Blink + talking animations in preview

**Files:**
- Modify: `tools/creature-designer.html`

This task adds the same blink state machine and talking sprite that the firmware uses, so the live preview shows actual on-device behavior.

- [ ] **Step 1: Add blink + talking sprite state**

Append to the `<script>` block:

```javascript
// ---------------------------------------------------------------------------
//  Animations: blinks + talking sprite
// ---------------------------------------------------------------------------
//
// Same parameters as creature_screen.c:
//   - blink interval: 2500–6500 ms random
//   - blink types weighted: 55% single, 25% double, 10% wink-l, 10% wink-r
//   - closed-eye renders as a 4×1 bottom-row bar (40×10 px)
//   - talking sprite cycles MOUTH_FRAMES at 150 ms/frame
//
// Animation state is global; preview re-renders every animation tick.

const TALKING_FRAMES = [
  // F0: closed bar
  [[0,0,0,0,0,0],[1,1,1,1,1,1],[0,0,0,0,0,0]],
  // F1: hollow O
  [[0,1,1,1,1,0],[1,0,0,0,0,1],[0,1,1,1,1,0]],
  // F2: filled
  [[1,1,1,1,1,1],[1,1,1,1,1,1],[1,1,1,1,1,1]],
];
const TALKING_CYCLE = [0, 1, 2, 1, 0, 2];
const TALKING_X = 130;
const TALKING_Y = 150;

let blinkClosedL = false;
let blinkClosedR = false;
let talkingFrameIdx = 0;

function scheduleNextBlink() {
  const wait = 2500 + Math.random() * 4000;
  setTimeout(runBlink, wait);
}
function runBlink() {
  const r = Math.random();
  let frames;
  if      (r < 0.55) frames = [{l:1,r:1,ms:120}, {l:0,r:0,ms:0}];
  else if (r < 0.80) frames = [{l:1,r:1,ms:90}, {l:0,r:0,ms:90}, {l:1,r:1,ms:90}, {l:0,r:0,ms:0}];
  else if (r < 0.90) frames = [{l:1,r:0,ms:220}, {l:0,r:0,ms:0}];
  else               frames = [{l:0,r:1,ms:220}, {l:0,r:0,ms:0}];

  let i = 0;
  function step() {
    blinkClosedL = !!frames[i].l;
    blinkClosedR = !!frames[i].r;
    refreshPreview();
    if (frames[i].ms === 0) {
      blinkClosedL = false;
      blinkClosedR = false;
      refreshPreview();
      scheduleNextBlink();
      return;
    }
    setTimeout(() => { i++; step(); }, frames[i].ms);
  }
  step();
}
scheduleNextBlink();

setInterval(() => {
  if (document.getElementById("talking").checked) {
    talkingFrameIdx = (talkingFrameIdx + 1) % TALKING_CYCLE.length;
    refreshPreview();
  }
}, 150);
```

- [ ] **Step 2: Update `refreshPreview()` to honor blink + talking state**

Replace the `refreshPreview()` function from the previous task with this version (which adds blink-bar rendering and the talking sprite):

```javascript
function refreshPreview() {
  const c = roster[activeIdx];
  const mood = document.getElementById("mood").value;
  const isTalking = document.getElementById("talking").checked;
  const color = MOOD_COLORS[mood] || "#0f0";

  previewWrap.innerHTML = "";

  // Brows
  for (const key of ["brow_l", "brow_r"]) {
    const slot = PREVIEW_SLOTS[key];
    for (let r = 0; r < slot.rows; r++) {
      for (let col = 0; col < slot.cols; col++) {
        if (!c[key][r][col]) continue;
        const div = document.createElement("div");
        div.className = "pixel";
        div.style.left   = (slot.x + col * PX) + "px";
        div.style.top    = (slot.y + r * PX + FACE_Y_OFFSET) + "px";
        div.style.width  = PX + "px";
        div.style.height = PX + "px";
        div.style.background = color;
        previewWrap.appendChild(div);
      }
    }
  }

  // Eyes — pixel grid if open, bottom-row bar if closed.
  function drawEye(key, closed) {
    const slot = PREVIEW_SLOTS[key];
    if (closed) {
      const div = document.createElement("div");
      div.className = "pixel";
      div.style.left   = slot.x + "px";
      div.style.top    = (slot.y + (slot.rows - 1) * PX + FACE_Y_OFFSET) + "px";
      div.style.width  = (slot.cols * PX) + "px";
      div.style.height = PX + "px";
      div.style.background = color;
      previewWrap.appendChild(div);
      return;
    }
    for (let r = 0; r < slot.rows; r++) {
      for (let col = 0; col < slot.cols; col++) {
        if (!c[key][r][col]) continue;
        const div = document.createElement("div");
        div.className = "pixel";
        div.style.left   = (slot.x + col * PX) + "px";
        div.style.top    = (slot.y + r * PX + FACE_Y_OFFSET) + "px";
        div.style.width  = PX + "px";
        div.style.height = PX + "px";
        div.style.background = color;
        previewWrap.appendChild(div);
      }
    }
  }
  drawEye("eye_l", blinkClosedL);
  drawEye("eye_r", blinkClosedR);

  // Mouth — bitmask if idle, talking sprite if talking.
  if (isTalking) {
    const frame = TALKING_FRAMES[TALKING_CYCLE[talkingFrameIdx]];
    for (let r = 0; r < 3; r++) {
      for (let col = 0; col < 6; col++) {
        if (!frame[r][col]) continue;
        const div = document.createElement("div");
        div.className = "pixel";
        div.style.left   = (TALKING_X + col * PX) + "px";
        div.style.top    = (TALKING_Y + r * PX + FACE_Y_OFFSET) + "px";
        div.style.width  = PX + "px";
        div.style.height = PX + "px";
        div.style.background = color;
        previewWrap.appendChild(div);
      }
    }
  } else {
    const slot = PREVIEW_SLOTS.mouth;
    for (let r = 0; r < slot.rows; r++) {
      for (let col = 0; col < slot.cols; col++) {
        if (!c.mouth[r][col]) continue;
        const div = document.createElement("div");
        div.className = "pixel";
        div.style.left   = (slot.x + col * PX) + "px";
        div.style.top    = (slot.y + r * PX + FACE_Y_OFFSET) + "px";
        div.style.width  = PX + "px";
        div.style.height = PX + "px";
        div.style.background = color;
        previewWrap.appendChild(div);
      }
    }
  }
}
```

- [ ] **Step 3: Reload and verify**

Refresh the page.

Expected:
- Periodic blinks happen every few seconds. Closed-eye = a 40×10 bar at the bottom of the eye area.
- Toggling the "Talking" checkbox: mouth-idle pixels disappear, the 6×3 talking sprite cycles through 3 frames at the on-device speed.

- [ ] **Step 4: Commit**

```bash
git add tools/creature-designer.html
git commit -m "designer: live blink + talking animations in preview"
```

---

## Task 9: Creature switching, name editing, clear-slot

**Files:**
- Modify: `tools/creature-designer.html`

- [ ] **Step 1: Wire up the navigation buttons + name input + new (clear) button**

Append to the `<script>` block:

```javascript
// ---------------------------------------------------------------------------
//  Roster navigation
// ---------------------------------------------------------------------------
function switchTo(idx) {
  if (idx < 0) idx = roster.length - 1;
  if (idx >= roster.length) idx = 0;
  activeIdx = idx;
  refreshEditor();
  refreshPreview();
}
document.getElementById("prev-creature").addEventListener("click", () => switchTo(activeIdx - 1));
document.getElementById("next-creature").addEventListener("click", () => switchTo(activeIdx + 1));

document.getElementById("creature-name").addEventListener("input", e => {
  roster[activeIdx].name = e.target.value;
});

document.getElementById("btn-new").addEventListener("click", () => {
  roster[activeIdx] = blankCreature(roster[activeIdx].name || `Slot ${activeIdx+1}`);
  refreshEditor();
  refreshPreview();
});
```

- [ ] **Step 2: Reload and verify**

Refresh the page. Click ◀ / ▶ to switch creatures. Edit the name field — value persists when you switch back. Click "New" — current creature clears to all zeros, name preserved.

- [ ] **Step 3: Commit**

```bash
git add tools/creature-designer.html
git commit -m "designer: roster navigation + name editing + clear"
```

---

## Task 10: Save / Load JSON + browser localStorage

**Files:**
- Modify: `tools/creature-designer.html`

- [ ] **Step 1: Add save (download), load (file picker), and localStorage auto-save**

Append to the `<script>` block:

```javascript
// ---------------------------------------------------------------------------
//  Persistence
// ---------------------------------------------------------------------------
const LS_KEY = "creature-designer-roster-v1";

function saveLocal() {
  try {
    localStorage.setItem(LS_KEY, JSON.stringify({ version: 1, creatures: roster }));
  } catch (e) {
    console.warn("localStorage save failed", e);
  }
}
function loadLocal() {
  try {
    const raw = localStorage.getItem(LS_KEY);
    if (!raw) return false;
    const obj = JSON.parse(raw);
    if (!obj || !Array.isArray(obj.creatures) || obj.creatures.length !== 6) return false;
    for (let i = 0; i < 6; i++) roster[i] = obj.creatures[i];
    return true;
  } catch (e) {
    console.warn("localStorage load failed", e);
    return false;
  }
}
loadLocal();
refreshEditor();
refreshPreview();

// Patch paintCell to also save to localStorage on every paint stroke.
const _paintCell = paintCell;
paintCell = function(el, value) {
  _paintCell(el, value);
  saveLocal();
};
// Also save on name edit, clear, and switch.
document.getElementById("creature-name").addEventListener("input", saveLocal);
document.getElementById("btn-new").addEventListener("click", saveLocal);

document.getElementById("btn-save").addEventListener("click", () => {
  const blob = new Blob([JSON.stringify({ version: 1, creatures: roster }, null, 2)],
                       { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = "creatures.json";
  a.click();
  URL.revokeObjectURL(url);
});

document.getElementById("btn-load").addEventListener("click", () => {
  const inp = document.createElement("input");
  inp.type = "file";
  inp.accept = "application/json,.json";
  inp.addEventListener("change", () => {
    const file = inp.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.addEventListener("load", () => {
      try {
        const obj = JSON.parse(reader.result);
        if (!obj || !Array.isArray(obj.creatures) || obj.creatures.length !== 6) {
          alert("Expected a roster JSON with 6 creatures.");
          return;
        }
        for (let i = 0; i < 6; i++) roster[i] = obj.creatures[i];
        saveLocal();
        switchTo(0);
      } catch (e) {
        alert("Invalid JSON: " + e.message);
      }
    });
    reader.readAsText(file);
  });
  inp.click();
});
```

Note: the line `paintCell = function(el, value) { ... }` requires that `paintCell` was declared with `let` or `var`, not `const`. Confirm Task 6 used `function paintCell(...)` (function declarations are reassignable). If you originally wrote `const paintCell = ...`, change it to `let` here.

- [ ] **Step 2: Reload and verify**

Refresh the page.

Expected:
- Paint some cells. Refresh the page. The cells reload from localStorage (auto-save works).
- Click Save — a `creatures.json` downloads with all 6 creatures.
- Click Load — pick the just-downloaded JSON. Roster restores.

- [ ] **Step 3: Commit**

```bash
git add tools/creature-designer.html
git commit -m "designer: save/load JSON + localStorage auto-save"
```

---

## Task 11: Export to C code

**Files:**
- Modify: `tools/creature-designer.html`

- [ ] **Step 1: Add the export modal + C generator**

Append to the `<script>` block:

```javascript
// ---------------------------------------------------------------------------
//  Export to C — generates a creatures_data.c array literal the user pastes
//  into the repo. Format mirrors the hand-authored file exactly so diffs
//  read cleanly when iterating.
// ---------------------------------------------------------------------------
function fmt2D(arr, indent) {
  const ind = " ".repeat(indent);
  return arr.map(row => `${ind}{${row.join(",")}}`).join(",\n");
}
function exportC() {
  const lines = [];
  lines.push("// src/creatures_data.c");
  lines.push("// Generated by tools/creature-designer.html.");
  lines.push("#include \"creatures_data.h\"");
  lines.push("");
  lines.push("const creature_data_t CREATURES_DATA[CREATURE_DATA_COUNT] = {");
  for (let i = 0; i < roster.length; i++) {
    const c = roster[i];
    lines.push(`    // ${i} — ${c.name}`);
    lines.push(`    {`);
    lines.push(`        .name = ${JSON.stringify(c.name)},`);
    lines.push(`        .brow_l = {`);
    lines.push(fmt2D(c.brow_l, 12));
    lines.push(`        },`);
    lines.push(`        .brow_r = {`);
    lines.push(fmt2D(c.brow_r, 12));
    lines.push(`        },`);
    lines.push(`        .eye_l  = {`);
    lines.push(fmt2D(c.eye_l, 12));
    lines.push(`        },`);
    lines.push(`        .eye_r  = {`);
    lines.push(fmt2D(c.eye_r, 12));
    lines.push(`        },`);
    lines.push(`        .mouth  = {`);
    lines.push(fmt2D(c.mouth, 12));
    lines.push(`        },`);
    lines.push(`    },`);
  }
  lines.push("};");
  return lines.join("\n");
}

document.getElementById("btn-export").addEventListener("click", () => {
  const code = exportC();
  // Show in a textarea modal with a copy button.
  const overlay = document.createElement("div");
  overlay.style.cssText = "position:fixed;inset:0;background:rgba(0,0,0,0.85);" +
                          "display:flex;align-items:center;justify-content:center;z-index:1000";
  const panel = document.createElement("div");
  panel.style.cssText = "background:#111;border:1px solid #333;padding:16px;" +
                        "width:80%;height:80%;display:flex;flex-direction:column";
  const ta = document.createElement("textarea");
  ta.value = code;
  ta.style.cssText = "flex:1 1 auto;background:#000;color:#0f0;" +
                     "font:12px ui-monospace,monospace;border:1px solid #333;padding:8px";
  const btnRow = document.createElement("div");
  btnRow.style.cssText = "margin-top:8px;display:flex;gap:8px";
  const copyBtn = document.createElement("button");
  copyBtn.textContent = "Copy";
  copyBtn.style.cssText = "background:#222;color:#0f0;border:1px solid #333;padding:4px 8px";
  copyBtn.addEventListener("click", () => {
    ta.select();
    document.execCommand("copy");
    copyBtn.textContent = "Copied!";
    setTimeout(() => { copyBtn.textContent = "Copy"; }, 1000);
  });
  const closeBtn = document.createElement("button");
  closeBtn.textContent = "Close";
  closeBtn.style.cssText = "background:#222;color:#0f0;border:1px solid #333;padding:4px 8px";
  closeBtn.addEventListener("click", () => overlay.remove());
  btnRow.appendChild(copyBtn);
  btnRow.appendChild(closeBtn);
  panel.appendChild(ta);
  panel.appendChild(btnRow);
  overlay.appendChild(panel);
  document.body.appendChild(overlay);
});
```

- [ ] **Step 2: Reload and verify**

Refresh the page. Paint some cells. Click "Export C". A modal appears with the generated C code. Click "Copy" — clipboard contains the code.

Sanity check: the generated code starts with `// src/creatures_data.c`, contains 6 entries, each with the right slot fields and the right dimensions.

- [ ] **Step 3: Commit**

```bash
git add tools/creature-designer.html
git commit -m "designer: export to creatures_data.c"
```

---

## Task 12: Pre-load existing 3 creatures as defaults

**Files:**
- Modify: `tools/creature-designer.html`

The designer should ship with the same 6 creatures the firmware ships with — 0/1/2 are the ports, 3/4/5 are placeholder copies. The user iterates from this baseline.

- [ ] **Step 1: Replace the `roster` initialization block**

Find:
```javascript
const roster = Array.from({length: 6}, (_, i) => blankCreature(`Slot ${i+1}`));
```

Replace with:

```javascript
const DEFAULT_ROSTER = [
  {
    name: "Daemon",
    brow_l: [[0,0,0,0],[0,0,0,0]],
    brow_r: [[0,0,0,0],[0,0,0,0]],
    eye_l:  [[0,1,1,0],[1,1,1,1],[1,1,1,1],[0,1,1,0]],
    eye_r:  [[0,1,1,0],[1,1,1,1],[1,1,1,1],[0,1,1,0]],
    mouth:  [[1,0,0,0,0,0,0,0,0,0,0,0,0,1],
             [0,1,0,0,0,0,0,0,0,0,0,0,1,0],
             [0,0,1,1,1,1,1,1,1,1,1,1,0,0]],
  },
  {
    name: "Laser",
    brow_l: [[1,0,0,0],[0,1,1,0]],
    brow_r: [[0,0,1,0],[1,1,0,0]],
    eye_l:  [[0,0,0,0],[0,0,0,0],[1,1,1,1],[0,0,0,0]],
    eye_r:  [[0,0,0,0],[0,0,0,0],[1,1,1,1],[0,0,0,0]],
    mouth:  [[1,0,0,0,0,0,0,0,0,0,0,0,0,0],
             [0,1,0,0,0,0,0,0,0,0,0,0,0,0],
             [0,0,1,1,1,1,1,1,1,1,1,1,0,0]],
  },
  {
    name: "Owl",
    brow_l: [[0,1,0,0],[1,0,1,0]],
    brow_r: [[0,1,0,0],[1,0,1,0]],
    eye_l:  [[1,1,1,1],[1,0,0,1],[1,0,0,1],[1,1,1,1]],
    eye_r:  [[1,1,1,1],[1,0,0,1],[1,0,0,1],[1,1,1,1]],
    mouth:  [[0,0,0,0,0,0,0,0,0,0,0,0,0,0],
             [0,0,0,0,0,0,0,0,0,0,0,0,0,0],
             [0,0,0,0,1,1,1,1,1,1,0,0,0,0]],
  },
  {
    name: "Slot 4",
    brow_l: [[0,0,0,0],[0,0,0,0]],
    brow_r: [[0,0,0,0],[0,0,0,0]],
    eye_l:  [[0,1,1,0],[1,1,1,1],[1,1,1,1],[0,1,1,0]],
    eye_r:  [[0,1,1,0],[1,1,1,1],[1,1,1,1],[0,1,1,0]],
    mouth:  [[1,0,0,0,0,0,0,0,0,0,0,0,0,1],
             [0,1,0,0,0,0,0,0,0,0,0,0,1,0],
             [0,0,1,1,1,1,1,1,1,1,1,1,0,0]],
  },
  {
    name: "Slot 5",
    brow_l: [[0,0,0,0],[0,0,0,0]],
    brow_r: [[0,0,0,0],[0,0,0,0]],
    eye_l:  [[0,1,1,0],[1,1,1,1],[1,1,1,1],[0,1,1,0]],
    eye_r:  [[0,1,1,0],[1,1,1,1],[1,1,1,1],[0,1,1,0]],
    mouth:  [[1,0,0,0,0,0,0,0,0,0,0,0,0,1],
             [0,1,0,0,0,0,0,0,0,0,0,0,1,0],
             [0,0,1,1,1,1,1,1,1,1,1,1,0,0]],
  },
  {
    name: "Slot 6",
    brow_l: [[0,0,0,0],[0,0,0,0]],
    brow_r: [[0,0,0,0],[0,0,0,0]],
    eye_l:  [[0,1,1,0],[1,1,1,1],[1,1,1,1],[0,1,1,0]],
    eye_r:  [[0,1,1,0],[1,1,1,1],[1,1,1,1],[0,1,1,0]],
    mouth:  [[1,0,0,0,0,0,0,0,0,0,0,0,0,1],
             [0,1,0,0,0,0,0,0,0,0,0,0,1,0],
             [0,0,1,1,1,1,1,1,1,1,1,1,0,0]],
  },
];

const roster = DEFAULT_ROSTER.map(c => JSON.parse(JSON.stringify(c)));
```

The deep-clone via JSON ensures the editor doesn't accidentally mutate `DEFAULT_ROSTER`.

- [ ] **Step 2: Add a "reset to defaults" button**

In the toolbar HTML, find:
```html
        <button id="btn-export">Export C</button>
```

Replace with:
```html
        <button id="btn-export">Export C</button>
        <button id="btn-reset">Reset</button>
```

In the `<script>` block, append:

```javascript
document.getElementById("btn-reset").addEventListener("click", () => {
  if (!confirm("Reset all 6 creatures to defaults? This wipes localStorage.")) return;
  for (let i = 0; i < 6; i++) {
    roster[i] = JSON.parse(JSON.stringify(DEFAULT_ROSTER[i]));
  }
  saveLocal();
  switchTo(0);
});
```

- [ ] **Step 3: Test on a clean browser state**

Clear localStorage in devtools (Application → Storage → Clear site data, or in console: `localStorage.removeItem("creature-designer-roster-v1")`). Reload.

Expected: Creatures 0/1/2 visible in their bitmask form. Cycling shows the original Daemon (round eyes + smile), Laser (slits + smirk + angry brows), and Owl (big eyes + flat + raised brows). Live preview matches the on-device renderings from Task 4.

- [ ] **Step 4: Commit**

```bash
git add tools/creature-designer.html
git commit -m "designer: ship with default roster matching firmware"
```

---

## Task 13: Per-slot clear button

**Files:**
- Modify: `tools/creature-designer.html`

A small quality-of-life: each slot needs a "Clear" button so the user can wipe one section without rebuilding the whole creature.

- [ ] **Step 1: Generate slot clear buttons**

In the `buildGrid()` function (Task 5), modify it to also add a Clear button next to each slot title. Find the function and replace it with:

```javascript
function buildGrid(slot) {
  const wrap = document.getElementById(`grid-${slot.key}`);
  // The .slot wrapper around this grid has the <h3> sibling — append a button there.
  const parent = wrap.parentElement;   // .slot
  const h3 = parent.querySelector("h3");
  if (h3 && !h3.querySelector(".clear-btn")) {
    const btn = document.createElement("button");
    btn.className = "clear-btn";
    btn.textContent = "clear";
    btn.style.cssText = "margin-left:8px;font-size:10px;background:#222;" +
                        "color:#0f0;border:1px solid #333;cursor:pointer";
    btn.addEventListener("click", () => {
      const c = roster[activeIdx];
      c[slot.key] = Array.from({length: slot.rows}, () => Array(slot.cols).fill(0));
      refreshEditor();
      refreshPreview();
      saveLocal();
    });
    h3.appendChild(btn);
  }

  wrap.style.gridTemplateColumns = `repeat(${slot.cols}, 28px)`;
  wrap.style.gridTemplateRows    = `repeat(${slot.rows}, 28px)`;
  wrap.innerHTML = "";
  for (let r = 0; r < slot.rows; r++) {
    for (let c = 0; c < slot.cols; c++) {
      const cell = document.createElement("div");
      cell.className = "cell";
      cell.dataset.slot = slot.key;
      cell.dataset.r = r;
      cell.dataset.c = c;
      wrap.appendChild(cell);
    }
  }
}
```

- [ ] **Step 2: Reload and verify**

Refresh the page. Each slot title now has a "clear" button. Clicking it wipes that slot for the active creature only.

- [ ] **Step 3: Commit**

```bash
git add tools/creature-designer.html
git commit -m "designer: per-slot clear buttons"
```

---

# Phase 3 — Designer-driven creature authoring (manual)

## Task 14: Author the 3 new creatures, paste, flash

**Files:** (depends on what the user designs)
- Modify: `src/creatures_data.c`

This task is the *creative* deliverable — it's the user designing the new faces. The plan executor (you, the engineer) prompts the user, then ships their work.

- [ ] **Step 1: Open the designer**

Run: `open /Users/yordanlasonov/Documents/GitHub/board-game/tools/creature-designer.html`

- [ ] **Step 2: Ask the user to design slots 4, 5, 6**

Tell the user: "The first three creatures are pre-loaded as ports of the existing on-device designs. Use ◀ / ▶ to switch to slots 4, 5, 6. Paint the new faces. Mood/talking toggles in the right panel preview behavior. Click 'Export C' when done."

Wait for the user to confirm they're done.

- [ ] **Step 3: Paste exported code into `src/creatures_data.c`**

Receive the exported code from the user. Replace the entire contents of `src/creatures_data.c` with the exported code.

- [ ] **Step 4: Verify it compiles**

Run: `cd /Users/yordanlasonov/Documents/GitHub/board-game && pio run`

Expected: clean build.

- [ ] **Step 5: Flash and verify on device**

Run: `cd /Users/yordanlasonov/Documents/GitHub/board-game && pio run -t upload`

Cycle through all 6 creatures. Verify the 3 new ones look like what was designed in the editor.

- [ ] **Step 6: Commit**

```bash
git add src/creatures_data.c
git commit -m "creatures: add three new designs from designer tool"
```

---

# Self-review checklist

After implementation, run through:

- [ ] All 6 creatures are visible on-device and cyclable.
- [ ] Blinks render correctly on every creature (bottom bar, not collapsed widget).
- [ ] Talking animation works on every creature (mouth-idle hides, sprite cycles, mouth-idle returns).
- [ ] Mood color tints all face widgets uniformly on every creature.
- [ ] Designer round-trips: design → export → paste → flash → on-device matches preview.
- [ ] Designer JSON save/load round-trips: save → load → identical state.
- [ ] localStorage survives a page refresh.
- [ ] No references to the old variant system remain in the codebase. Run:
  ```bash
  cd /Users/yordanlasonov/Documents/GitHub/board-game && grep -rn \
    -e eye_variant_t -e mouth_variant_t -e brow_variant_t \
    -e apply_eye_variant -e apply_mouth_variant -e apply_brow_variant \
    -e s_pupil_l -e s_pupil_r -e s_pupils_present \
    -e s_smile -e k_smile -e k_brow_x -e k_brow_y \
    src/
  ```
  Expected: no matches. If any line is found, it's leftover from the refactor — remove it.
- [ ] `CREATURE_COUNT` is 6 in `devcfg.c`.
- [ ] `src/CMakeLists.txt` includes `creatures_data.c` in `SRCS`.
