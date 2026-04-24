// ---------------------------------------------------------------------------
//  Menu screen — hub reached by swiping left from the creature.
//
//  Three tappable rows:
//    Wallet  → wallet screen (QR + balances)
//    Info    → device info (IP, firmware, heap, MAC, uptime)
//    Config  → model + voice picker, persisted to NVS
//
//  Swipe right returns to the creature. Same status-bar convention as the
//  other side screens.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

bool      menu_screen_init(void);
lv_obj_t *menu_screen(void);

void menu_screen_set_status(const char *s);
void menu_screen_set_price(const char *s);
void menu_screen_set_usdc(const char *s);

#ifdef __cplusplus
}
#endif
