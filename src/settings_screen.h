// ---------------------------------------------------------------------------
//  Settings screen.
//
//  Rows:
//    • Wi-Fi    — row is tappable; shows current SSID + IP + RSSI.
//                 Tapping fires the callback the caller registered with
//                 settings_screen_on_wifi_click().
//    • Bluetooth — lv_switch bound to devcfg_bluetooth()/set.
//    • Volume    — lv_slider (0..21) bound to devcfg_volume()/set.
//    • Brightness — lv_slider (0..255) bound to devcfg_brightness()/set.
//                   Applies immediately via devcfg_set_brightness()'s LEDC
//                   path so the user sees the change as they drag.
//
//  The status bar mirrors the creature/wallet screens so the main loop
//  can broadcast status/price/usdc to every screen uniformly.
//
//  Call settings_screen_refresh() whenever wifi_sta_* state or devcfg
//  values change from outside this screen (e.g. after a successful
//  wifi_sta_connect from the wifi screen) so the SSID row updates.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*settings_wifi_click_cb_t)(void);

bool settings_screen_init(void);
lv_obj_t *settings_screen(void);

// Status-bar setters (same API shape as creature/wallet screens).
void settings_screen_set_status(const char *s);
void settings_screen_set_price(const char *s);
void settings_screen_set_usdc(const char *s);

// Re-read devcfg + wifi_sta state and update the controls + Wi-Fi row.
// Safe to call from the main loop.
void settings_screen_refresh(void);

// Install the Wi-Fi row click callback. Typically wired to "navigate to
// wifi_screen". `cb` may be NULL to clear.
void settings_screen_on_wifi_click(settings_wifi_click_cb_t cb);

#ifdef __cplusplus
}
#endif
