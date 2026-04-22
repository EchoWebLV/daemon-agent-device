#pragma once
// ---------------------------------------------------------------------------
//  LVGL bring-up + screen wiring.
//
//  `ui_init()` mounts LVGL on top of esp_lcd and builds the four Daemon
//  screens (creature / wallet / settings / wifi). It leaves the creature
//  screen loaded so boot lands on the face.
//
//  After init, the app main loop is expected to call:
//
//    ui_set_status("idle")          — short left-of-status-bar text
//    ui_set_price("SOL $198.42")    — right-of-status-bar ticker
//    ui_set_usdc("USDC 12.34")      — below the ticker
//    ui_tick()                      — ~25 Hz; drives creature animations
//    ui_refresh_wallet() / ui_refresh_settings()
//                                   — after a wallet refresh / settings change
//
//  Navigation (swipe to cycle screens, back-to-creature from wifi on
//  connect) is wired in phase 7's integration layer; for now the only
//  screen visible is the creature, and tapping the Wi-Fi row in settings
//  is the one explicit transition.
// ---------------------------------------------------------------------------
#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#include "creature_screen.h"   // creature_mood_t

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_init(void);

// Switch to a specific screen (wraps lv_screen_load + the per-screen
// "just became visible" hook). Safe to call from the main loop.
void ui_show_creature(void);
void ui_show_wallet(void);
void ui_show_settings(void);
void ui_show_wifi(void);

// Broadcast status-bar values to every screen so the ticker reads the
// same everywhere regardless of which one the user lands on.
void ui_set_status(const char *s);
void ui_set_price(const char *s);
void ui_set_usdc(const char *s);

// Creature subtitle + mood + talking mouth.
void ui_set_subtitle(const char *s);
typedef int ui_mood_t;   // re-exports creature_mood_t through a stable alias
void ui_set_mood(ui_mood_t m);
void ui_set_talking(bool on);

// Call periodically (~25 Hz) to drive mouth animation etc.
void ui_tick(void);

// Re-read subsystem state and redraw the relevant screens. Cheap — safe
// to call on the usual 30-60 s wallet/price cadence.
void ui_refresh_wallet(void);
void ui_refresh_settings(void);

#ifdef __cplusplus
}
#endif
