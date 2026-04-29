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

typedef struct {
    const char *name;
    uint8_t brow_l[2][4];
    uint8_t brow_r[2][4];
    uint8_t eye_l [4][4];
    uint8_t eye_r [4][4];
    uint8_t mouth [3][14];
} creature_data_t;

// The roster lives in creatures_data.c. CREATURE_DATA_COUNT is computed there
// from the array's actual size, so adding/removing rows in the .c file is the
// only edit needed — this header stays stable.
extern const creature_data_t CREATURES_DATA[];
extern const int CREATURE_DATA_COUNT;
