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

// LLM model id (uses the same provider/name format as the Chrome
// extension, e.g. "google/gemini-3.1-pro", "openai/gpt-4o-mini").
String  devcfgLlmModel();
void    devcfgSetLlmModel(const String &m);

// Custom personality — injected into the AI system prompt in place of the
// default "You are Daemon..." text. Empty string means "use default".
String  devcfgPersonality();
void    devcfgSetPersonality(const String &p);

// JSON arrays stored as strings (so we can ship them straight to the web
// UI). Contents:
//   svc_enabled: array of service ids the user has flipped on
//   svc_custom:  array of full X402Service JSON objects added via .md
String  devcfgServicesEnabled();
void    devcfgSetServicesEnabled(const String &jsonArray);
String  devcfgCustomServices();
void    devcfgSetCustomServices(const String &jsonArray);
