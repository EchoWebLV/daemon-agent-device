// ---------------------------------------------------------------------------
//  Wi-Fi picker screen.
//
//  Provides:
//    • a [Scan] button that fires an async scan via wifi_sta_scan()
//    • a scrollable list of networks (SSID + signal strength)
//    • a password-entry modal with an lv_keyboard for protected networks
//
//  Scans and connect attempts both block for seconds at a time — they run
//  on dedicated FreeRTOS tasks and marshal results back to the LVGL task
//  via lv_async_call so we never stall the display.
//
//  On a successful connect, credentials are persisted by wifi_sta_connect()
//  (which calls devcfg_set_wifi) and the caller's "connected" callback runs
//  so the host state machine can return to the previous screen.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_screen_connected_cb_t)(void);

bool wifi_screen_init(void);
lv_obj_t *wifi_screen(void);

// Status-bar setters (shared with the other screens).
void wifi_screen_set_status(const char *s);
void wifi_screen_set_price(const char *s);
void wifi_screen_set_usdc(const char *s);

// Fire a scan without the user having to tap the button. Useful to call
// right after the screen becomes visible so the list is populated.
void wifi_screen_kick_scan(void);

// Register a callback that fires when a connect attempt succeeds. The
// callback runs on the LVGL task (dispatched via lv_async_call). `cb` may
// be NULL to clear.
void wifi_screen_on_connected(wifi_screen_connected_cb_t cb);

#ifdef __cplusplus
}
#endif
