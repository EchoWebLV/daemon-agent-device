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

// LLM model id (uses the same provider/model format as the Chrome
// extension + sol.blockrun.ai, e.g. "google/gemini-3.1-pro",
// "openai/gpt-4o-mini"). Empty string means "use the default baked in".
const char *devcfg_llm_model(void);
void        devcfg_set_llm_model(const char *model);

// ElevenLabs voice id (e.g. "pFZP5JQG7iQjIQuC4Bku"). Empty string means
// "fall back to ELEVENLABS_VOICE_ID from secrets.h".
const char *devcfg_voice_id(void);
void        devcfg_set_voice_id(const char *voice_id);

// Custom personality. Empty means "use the built-in persona in ai.c".
// The string is injected at the head of the AI system prompt on every
// chat round-trip.
const char *devcfg_personality(void);
void        devcfg_set_personality(const char *persona);

// Custom x402 services library + enabled-service IDs. Both are stored as
// raw JSON strings so the phone UI (and chrome-ext, if it syncs later) can
// round-trip them without the device needing to re-serialise. Getters
// always return a valid JSON string — "[]" when nothing has been saved.
//
// customServices is capped at 4096 bytes (≈8 services of typical size);
// servicesEnabled at 512 bytes. Over-cap writes are rejected silently.
const char *devcfg_custom_services(void);
void        devcfg_set_custom_services(const char *json);

const char *devcfg_services_enabled(void);
void        devcfg_set_services_enabled(const char *json);

#ifdef __cplusplus
}
#endif
