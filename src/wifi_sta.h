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

// SSID of the currently-associated AP, or "" if not connected. Pointer
// valid until the next (re)connect.
const char *wifi_sta_current_ssid(void);

// Signal strength of the currently-associated AP, or 0 if not connected.
int8_t wifi_sta_current_rssi(void);

// Blocking AP scan. Fills `out` with up to `cap` entries; returns the
// number written (0..cap). The driver's own result list can be longer
// — we just truncate to fit the caller's array. Duplicates (same SSID
// seen on multiple channels) are collapsed, keeping the strongest RSSI.
// Safe to call while connected; the radio briefly leaves the BSS but
// auto-rejoins when the scan ends. Blocks ~2-3 s for the default active
// scan sweep across all 2.4 GHz channels.
typedef struct {
    char    ssid[33];       // NUL-terminated, up to 32 bytes
    int8_t  rssi;           // dBm (negative)
    uint8_t auth_open;      // 1 if WIFI_AUTH_OPEN, else 0 (WPA/WPA2/etc.)
} wifi_sta_scan_ap_t;

size_t wifi_sta_scan(wifi_sta_scan_ap_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
