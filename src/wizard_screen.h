// ---------------------------------------------------------------------------
//  Wizard splash. Shown on the LCD during first-run setup so the user can
//  see the AP SSID + the URL to visit on their phone.
// ---------------------------------------------------------------------------
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Render the splash with the given SSID (e.g. "daemon-setup-10D5"). The
// screen is loaded synchronously — caller must already hold the LVGL lock
// or be running on the LVGL task.
void wizard_screen_show(const char *ssid);

#ifdef __cplusplus
}
#endif
