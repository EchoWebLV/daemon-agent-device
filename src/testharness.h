#pragma once
// ---------------------------------------------------------------------------
//  Host-driven E2E test harness.
//
//  When the laptop sends `TEST BEGIN\n` over the USB-CDC serial link, the
//  firmware enters test mode and responds to a small verb set. The protocol
//  and full case list live in
//      docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md
//
//  Design notes specific to ESP-IDF (the spec was drafted against Arduino):
//    * Always compiled in. Boot cost is one FreeRTOS task blocked on
//      stdin — no polling in app_main's loop.
//    * Reads one line at a time with fgets(stdin). PlatformIO's default
//      sdkconfig routes stdio to the USB-CDC console so this Just Works.
//    * Every response line starts with `TEST OK ` or `TEST ERR ` and is
//      terminated with a single `\n`. The host reads line-by-line.
//    * Regular ESP_LOGx/printf diagnostic output is still free to flow
//      through the same link; the host ignores lines that don't begin
//      with `TEST `.
//
//  Boot order: call test_harness_begin() after ui_init() + wifi_sta_begin()
//  so every verb's backing subsystem is already up.
// ---------------------------------------------------------------------------
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Spawn the harness task. Returns true on success. The task idles until it
// sees "TEST BEGIN" — at rest it costs ~3 KB of stack and a single blocked
// fgets() call, so it's fine to leave installed in shipping builds.
bool test_harness_begin(void);

// True while the host has sent BEGIN but not yet END. Other modules can
// consult this to suppress actions that would interfere with a test run
// (e.g. refreshing wallet/price state from a separate task). None of the
// subsystems currently need to gate on this — it's exposed for future use.
bool test_harness_in_test_mode(void);

#ifdef __cplusplus
}
#endif
