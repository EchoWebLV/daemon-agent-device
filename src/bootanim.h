// ============================================================================
//  Boot + Wi-Fi join animations.
//
//  Two cinematic pieces that play before the main creature view takes over:
//
//    1. CRT power-on  — a horizontal scanline blooms from the center of
//       the screen, pulses bright, then stretches vertically to fill the
//       display, flashes near-white, and fades to black. Duration + tint
//       are picked from the ESP32 reset reason so a fresh power-on gets
//       the full ~820 ms cinematic pass, while a crash-reboot gets a
//       short red blip that tells you at a glance what happened. Drawn
//       into a PSRAM-backed full-screen sprite so every frame is double-
//       buffered and flicker-free, then pushed to the ST7789 at ~30 FPS.
//
//    2. Radar wifi-join — concentric cyan rings emanate from the middle
//       of the screen, one fresh ring every ~450 ms, each fading as it
//       expands. SSID line + animated "connecting…" dots + a 25-second
//       progress bar along the bottom. Closes with either a single
//       expanding "CONNECTED" flash or a red collapsing "OFFLINE" ring
//       on timeout. Must be ticked from inside server.cpp's connect
//       wait loop (serverBeginWifi's optional frame-callback overload).
//
//  Both animations share one 240×320×16 bpp PSRAM sprite that's
//  allocated by `bootAnimPlayPowerOn()` and released by
//  `wifiJoinAnimEnd()`. Total PSRAM cost is ~150 KB, returned to the
//  pool before loop() ever runs.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

// Play the CRT power-on animation on `tft`. `resetReason` is the raw
// esp_reset_reason_t value — used to pick the tint (red for panic,
// amber for brownout, orange for WDT, cyan for clean boot) and the
// total duration (short for errors, full ~820 ms for power-on).
// Blocks for the duration of the animation; caller should ensure the
// LCD has been initialised (`tft.init()`, `tft.fillScreen(0)`). Safe
// to call before any other sprite has been allocated.
void bootAnimPlayPowerOn(TFT_eSPI *tft, int resetReason);

// Begin the radar wifi-join animation. Does not block — just paints
// the first frame (title + initial ring + empty progress bar). Call
// `wifiJoinAnimTick(elapsedMs)` at ~30 FPS (e.g. from inside the
// connect wait loop) to advance it, and `wifiJoinAnimEnd(ok)` when
// the connect attempt resolves.
void wifiJoinAnimBegin(TFT_eSPI *tft, const String &ssid);
void wifiJoinAnimTick(uint32_t elapsedMs);
void wifiJoinAnimEnd(bool success);
