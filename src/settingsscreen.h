// ============================================================================
//  Settings panel — reached by pulling DOWN from the top edge of any other
//  screen. Provides Wi-Fi info, Bluetooth toggle, Volume slider, and LCD
//  Brightness slider. Swipe UP to close.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool settingsScreenBegin(TFT_eSPI *tft);
void settingsScreenDraw();   // full repaint
void settingsScreenTick();   // input + incremental repaint (call each frame)

// Re-render the entire settings panel into an off-screen sprite for the
// slide-transition module.
void settingsScreenDrawTo(TFT_eSprite *target);

// Returns true exactly once after the user tapped the Wi-Fi row, so the
// caller can push the Wi-Fi management screen.
bool settingsScreenConsumeWifiTap();

// Returns true exactly once after the user tapped the top-right X button.
// Caller should close the Settings panel.
bool settingsScreenConsumeClose();
