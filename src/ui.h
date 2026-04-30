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
//    ui_set_status("idle")          — no-op today; kept so callers that
//                                     used to push the IP into the top bar
//                                     keep compiling. The creature's top
//                                     bar now shows USDC (left) and SOL
//                                     price (right); side screens show
//                                     their name + an X close button.
//    ui_set_price("SOL $198.42")    — right-of-creature-status-bar ticker
//    ui_set_usdc("USDC 12.34")      — left-of-creature-status-bar ticker
//    ui_tick()                      — ~25 Hz; drives creature animations
//    ui_refresh_wallet() / ui_refresh_settings()
//                                   — after a wallet refresh / settings change
//
//  Navigation (vertical swipes only):
//    creature  -- swipe up    --> menu  (Wallet / Info / Config)
//    creature  -- swipe down  --> settings --> tap Wi-Fi row --> wifi
//    menu      -- swipe down  --> creature
//                  -- tap row --> wallet | info | config
//    wallet    -- swipe down  --> menu
//    info      -- swipe down  --> menu
//    config    -- swipe down  --> menu
//    settings  -- swipe up    --> creature
//    wifi      -- swipe down (or on-connect) --> settings
//  Swipe handlers are installed by ui_init() on each screen's root.
// ---------------------------------------------------------------------------
#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#include "creature_screen.h"   // creature_mood_t

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_init(void);

// Call after touch_init() so LVGL's pointer indev exists. Lowers the swipe
// threshold from the stock 50 px down to 25 px — makes vertical gestures on
// the 240x320 panel reliable without having to reach across the whole screen.
void ui_tune_gestures(void);

// Switch to a specific screen (wraps lv_screen_load_anim + the per-screen
// "just became visible" hook). Transitions slide at ~220 ms in the direction
// implied by the semantic screen layout (wallet←creature→settings, wifi
// below settings). Safe to call from the main loop.
void ui_show_creature(void);
void ui_show_menu(void);
void ui_show_wallet(void);
void ui_show_info(void);
void ui_show_config(void);
void ui_show_settings(void);
void ui_show_wifi(void);

// Broadcast status-bar values to every screen so the ticker reads the
// same everywhere regardless of which one the user lands on.
void ui_set_status(const char *s);
void ui_set_price(const char *s);
void ui_set_usdc(const char *s);

// Apply a theme live — no reboot. Tears down all cached screens, applies
// the new palette, rebuilds them, and reloads whichever screen was active.
// Safe to call from inside an LVGL event handler — defers via lv_async_call
// so the firing widget isn't destroyed under its own feet.
typedef enum { UI_THEME_DARK = 0, UI_THEME_LIGHT = 1 } ui_theme_t;
void ui_apply_theme(ui_theme_t theme);

// Creature subtitle + mood + talking mouth.
void ui_set_subtitle(const char *s);
typedef int ui_mood_t;   // re-exports creature_mood_t through a stable alias
void ui_set_mood(ui_mood_t m);
void ui_set_talking(bool on);

// Call periodically (~25 Hz) to drive mouth animation etc.
void ui_tick(void);

// --- Test-harness bridges --------------------------------------------------
// Exposed to src/testharness.c so the host-driven smoke tests can drive the
// UI without having to talk LVGL directly. None of these are called from
// normal app code; they only exist to give the harness a narrow, deterministic
// surface. Safe from any task — each grabs the lvgl_port lock internally.
//
// ui_current_screen_name(): short name for whichever screen is currently
// loaded ("creature" / "wallet" / "settings" / "wifi" / "unknown"). Pointer
// returns a static string literal; do not free.
const char *ui_current_screen_name(void);

// Synthesize a swipe on the currently-active screen using the same mapping
// as the LVGL gesture listeners in ui.c. Direction values: 0=LEFT, 1=RIGHT,
// 2=UP, 3=DOWN. Unknown values are a no-op.
void ui_inject_swipe(int direction);

// Force a synchronous repaint of the active screen. Invalidates the root and
// runs one LVGL refresh cycle inside the port lock so the caller can time
// the paint with esp_timer_get_time() around the call.
void ui_force_repaint(void);

// Re-read subsystem state and redraw the relevant screens. Cheap — safe
// to call on the usual 30-60 s wallet/price cadence.
void ui_refresh_wallet(void);
void ui_refresh_settings(void);
void ui_refresh_info(void);
void ui_refresh_config(void);

// Called by the AI /say handler once a reply is ready. Updates the
// creature's subtitle + mood, triggers voice_speak(), and stays on the
// creature screen so the user sees the face while Daemon talks.
//
// The call itself is cheap and returns immediately — audio plays on the
// voice task. While the reply is being spoken the creature's mouth
// animates via the internal lv_timer.
void ui_deliver_reply(const char *text);

#ifdef __cplusplus
}
#endif
