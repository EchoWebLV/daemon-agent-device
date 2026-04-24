// ---------------------------------------------------------------------------
//  Info screen — read-only device stats.
//
//  Shows: firmware version + build date, IDF version, chip + core count,
//  MAC, IP, free heap (internal + PSRAM), uptime.
//
//  The main loop calls info_screen_refresh() periodically so dynamic
//  values (IP, heap, uptime) stay current.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

bool      info_screen_init(void);
lv_obj_t *info_screen(void);

void info_screen_set_status(const char *s);
void info_screen_set_price (const char *s);
void info_screen_set_usdc  (const char *s);

// Re-read dynamic values (IP, heap, uptime) and redraw. Cheap — safe on
// the usual wallet-refresh cadence.
void info_screen_refresh(void);

#ifdef __cplusplus
}
#endif
