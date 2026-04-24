// ---------------------------------------------------------------------------
//  Config screen — model + TTS voice picker.
//
//  Two sections in one scrollable list:
//
//    MODEL
//      > DeepSeek V3
//      > GPT-4o Mini
//      ...
//    VOICE
//      > Rachel
//      > Adam
//      ...
//
//  The currently-selected row in each section shows a "> " prefix (green);
//  tapping another row re-selects it and persists the id via devcfg.
//  The ai.c and voice.c paths read the new value the next time they
//  build a request, so no restart is required.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

bool      config_screen_init(void);
lv_obj_t *config_screen(void);

void config_screen_set_status(const char *s);
void config_screen_set_price (const char *s);
void config_screen_set_usdc  (const char *s);

// Re-read persisted selections from NVS and redraw the selected indicators.
// Safe to call from the main loop; cheap.
void config_screen_refresh(void);

#ifdef __cplusplus
}
#endif
