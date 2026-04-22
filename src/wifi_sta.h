// ---------------------------------------------------------------------------
//  Wi-Fi STA bring-up. Single network at a time, no SoftAP here (we may add
//  provisioning later in a separate module).
//
//  The usual flow on boot:
//      wifi_sta_init();                   // once, before any connect attempt
//      wifi_sta_begin();                  // tries NVS creds, then secrets.h
//      if (wifi_sta_is_connected()) ...   // non-blocking check
//
//  Swapping networks at runtime (e.g. from the Wi-Fi settings screen):
//      wifi_sta_drop();
//      wifi_sta_connect("myssid", "mypass", 15000);
//  On success `wifi_sta_connect` persists the new creds via devcfg.
// ---------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register WiFi event handlers, create the netif, and initialise the WiFi
// driver in STA mode. Does not start the driver — wifi_sta_connect does
// that. Safe to call multiple times; subsequent calls are no-ops.
esp_err_t wifi_sta_init(void);

// Load credentials from NVS (via devcfg). If none present, fall back to the
// compile-time WIFI_SSID/WIFI_PASSWORD from secrets.h. Blocks for up to
// `timeout_ms` milliseconds waiting for an IP. Returns ESP_OK on success.
esp_err_t wifi_sta_begin(uint32_t timeout_ms);

// Attempt a connection to a specific SSID/password. On success, persists
// the credentials to NVS so the next boot picks them up. Blocks up to
// `timeout_ms`. Safe to call while already connected (will disconnect and
// retry with the new creds).
esp_err_t wifi_sta_connect(const char *ssid, const char *password,
                           uint32_t timeout_ms);

// Drop the current association without clearing stored credentials.
void wifi_sta_drop(void);

// True when we currently hold an IP from the AP.
bool wifi_sta_is_connected(void);

// Copies the current IPv4 address as a dotted string into `out` (e.g.
// "192.168.1.17"). Returns "" when not connected. `cap` must be >= 16.
void wifi_sta_ip_str(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
