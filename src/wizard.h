// ---------------------------------------------------------------------------
//  First-boot captive-portal wizard. Phase 2c.
//
//  Brings up an unsecured Wi-Fi AP named "daemon-setup-XXXX" (last 4 of
//  the device pubkey), serves a tiny HTML form at the root URL where the
//  user can paste their Wi-Fi credentials + Phantom owner pubkey + PIN.
//  Saves to NVS (via devcfg) and reboots into normal STA mode.
//
//  Replaces secrets.h-driven config for shipping units — secrets.h stays
//  available as a dev / overrides path.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// True if no NVS-backed owner_pubkey has been provisioned yet. Caller
// uses this at boot to decide between starting the wizard vs. coming up
// in STA mode normally.
bool wizard_is_first_run(void);

// Mark the wizard as completed (writes a flag to NVS). Idempotent.
void wizard_mark_done(void);

// Spin up a Wi-Fi AP + HTTP server on it and register wizard URI handlers.
// Caller is responsible for stopping any existing STA connection first
// — calling this with STA active will switch to APSTA. After the user
// submits the form, wizard_mark_done() is called automatically and the
// device reboots into STA mode.
esp_err_t wizard_start(void);

// Tear down the AP + handlers. Safe to call when not started.
esp_err_t wizard_stop(void);

// On-the-wire form fields. Persisted via devcfg for Wi-Fi + a new NVS
// key for owner_pubkey + pin_setup() for the PIN. Exposed for tests.
typedef struct {
    char ssid[33];
    char password[65];
    char owner_pubkey[48];
    char pin[33];
} wizard_form_t;

// Persist the form fields to NVS. PIN goes through pin_setup() so the
// existing wallet seed gets sealed under the chosen PIN. Returns ESP_OK
// on success, an error code otherwise.
esp_err_t wizard_apply_form(const wizard_form_t *form);

// Read the NVS-backed owner pubkey, if any. Returns NULL if unset (the
// wallet falls back to secrets.h's OWNER_PUBKEY in that case).
const char *wizard_owner_pubkey(void);

// Compute the AP SSID this device would broadcast — "daemon-setup-XXXX"
// where XXXX is the last 4 hex of the SoftAP MAC. Stable across reboots.
// Caller-owned buffer, returns true on success.
bool wizard_compute_ap_ssid(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
