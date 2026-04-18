// ============================================================================
//  Device-level settings: volume, LCD brightness, Bluetooth toggle.
//  All values persist across reboots in NVS (ESP32 Preferences API) and
//  are applied immediately to the relevant hardware on set.
// ============================================================================
#pragma once
#include <Arduino.h>

void    devcfgBegin();                  // load from NVS + apply to HW

// Volume: 0..21 (forwarded to the ESP32-audioI2S library).
uint8_t devcfgVolume();
void    devcfgSetVolume(uint8_t v);

// Brightness: 0..255 (PWM duty on the LCD backlight pin). Clamped to
// a small minimum so the screen is never completely black.
uint8_t devcfgBrightness();
void    devcfgSetBrightness(uint8_t b);

// Bluetooth toggle. Today this just flips a persisted flag; no BLE stack
// is started yet. Hook up when a real feature needs it.
bool    devcfgBluetooth();
void    devcfgSetBluetooth(bool on);

// Wi-Fi credentials. Empty string means "use the compile-time fallback
// from secrets.h" (only matters on first boot). Setting these persists
// the new SSID/password so the user can change networks from the UI.
String  devcfgWifiSSID();
String  devcfgWifiPassword();
void    devcfgSetWifi(const String &ssid, const String &password);
void    devcfgClearWifi();
