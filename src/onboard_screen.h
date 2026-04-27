// ---------------------------------------------------------------------------
//  On-device first-run wizard.
//
//  Replaces the captive-portal AP + WebSerial paths with a wizard that
//  runs entirely on the device's LCD. State machine:
//
//      Welcome  →  Wi-Fi list  →  Wi-Fi password  →  Connecting...
//             →  PIN (first)  →  PIN (confirm)    →  Done (reboot)
//
//  Reuses wifi_sta_scan / wifi_sta_connect for the network side and
//  pin_screen_open for the keypad UI. On success, calls
//  wizard_apply_form (which seals the freshly-generated seed under the
//  PIN, persists Wi-Fi creds, and marks the wizard done) before the
//  caller reboots.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Run the wizard to completion. Blocks the calling task on an internal
// semaphore until the user finishes provisioning OR cancels with the X.
// Returns true on success (caller should esp_restart afterwards) and
// false if the user backed out before committing — caller can either
// retry (call again) or take some other recovery path.
//
// Pre-condition: the LVGL port and display must already be initialized.
// Touch must be alive too (the wizard is touch-driven).
bool onboard_screen_run(void);

#ifdef __cplusplus
}
#endif
