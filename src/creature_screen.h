// ---------------------------------------------------------------------------
//  Home screen: the animated Daemon creature + a subtitle for chat replies.
//
//  LVGL v9 widgets replace the hand-drawn TFT_eSPI silhouette from the
//  Arduino build. The vibe stays the same (rounded blue body, two ears,
//  pulsing eyes, a mouth that opens when talking) but we lean on LVGL's
//  styling + animations rather than pixel blitting.
//
//  Call `creature_screen_init()` once at boot. The returned root widget
//  is the screen object you pass to `lv_screen_load()` when navigation
//  wants to show Daemon.
//
//  Everything that mutates the visible state (status, price, usdc,
//  subtitle, mood, talking) must be called with the lvgl_port lock held
//  — or from inside an LVGL event callback, which implicitly holds it.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CREATURE_MOOD_IDLE   = 0,
    CREATURE_MOOD_LISTEN = 1,
    CREATURE_MOOD_THINK  = 2,
    CREATURE_MOOD_TALK   = 3,
    CREATURE_MOOD_HAPPY  = 4,
    CREATURE_MOOD_ANGRY  = 5,
} creature_mood_t;

// Build the widget tree. Safe to call multiple times; subsequent calls
// are no-ops.
bool creature_screen_init(void);

// Root object — pass to lv_screen_load() to show Daemon.
lv_obj_t *creature_screen(void);

// Top status bar: short left text (IP, "thinking", …) + right ticker
// (SOL price) + secondary right line (USDC balance).
void creature_screen_set_status(const char *s);
void creature_screen_set_price(const char *s);
void creature_screen_set_usdc(const char *s);

// Subtitle under Daemon. Word-wraps; pass "" to hide.
void creature_screen_set_subtitle(const char *text);

// Mood + talking. Talking animates the mouth independently of mood.
void creature_screen_set_mood(creature_mood_t m);
void creature_screen_set_talking(bool on);

// Tick called from the app main loop — drives mouth animation.
void creature_screen_tick(void);

// Jitter the face left/right for ~1.2 s. No-op if a shake is already in
// flight. Safe from any task — takes the lvgl_port lock internally.
void creature_screen_shake(void);

// Cycle the active creature variant by `delta` (typically +1 or -1). The
// new index is stored in NVS via devcfg, the face is reshaped to the
// variant's eye / mouth / brow trait combo, and the current mood colour
// is re-applied so freshly-shown widgets don't flash white. Wraps around
// modulo CREATURE_COUNT.
void creature_screen_cycle(int delta);

#ifdef __cplusplus
}
#endif
