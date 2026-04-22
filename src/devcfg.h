// ---------------------------------------------------------------------------
//  Device-level settings + LCD backlight PWM.
//
//  All values persist across reboots in NVS under the "daemon" namespace.
//  Current working set is deliberately minimal — we'll grow it as later
//  phases (AI, custom services) need more keys.
//
//  String getters return a pointer to an internal cache. The pointer is
//  valid until the next setter on that same key; callers that need to hold
//  onto a value across a setter call should copy it.
// ---------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise NVS namespace + LEDC backlight + load persisted values.
// Applies brightness to the backlight immediately. Safe to call more than
// once; subsequent calls are no-ops.
esp_err_t devcfg_init(void);

// Volume: 0..21. Stored only; voice module reads it on init.
uint8_t devcfg_volume(void);
void    devcfg_set_volume(uint8_t v);

// Brightness: 0..255 PWM duty on the LCD backlight pin. Applied to the
// backlight immediately on set. Clamped to a small minimum so the panel
// is never completely dark.
uint8_t devcfg_brightness(void);
void    devcfg_set_brightness(uint8_t b);

// Bluetooth toggle. Today just a persisted flag; no BLE stack yet.
bool    devcfg_bluetooth(void);
void    devcfg_set_bluetooth(bool on);

// Wi-Fi credentials. Empty string means "none stored — fall back to the
// compile-time defaults in secrets.h". Setting persists the new values.
const char *devcfg_wifi_ssid(void);
const char *devcfg_wifi_password(void);
void        devcfg_set_wifi(const char *ssid, const char *password);
void        devcfg_clear_wifi(void);

#ifdef __cplusplus
}
#endif
