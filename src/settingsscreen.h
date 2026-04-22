// ============================================================================
//  Settings panel — reached by pulling DOWN from the top edge of any other
//  screen. Provides Wi-Fi info, Bluetooth toggle, Volume slider, and LCD
//  Brightness slider. Swipe UP to close.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool settingsScreenBegin(TFT_eSPI *tft);

// Called by main.cpp every time Settings becomes the active screen.
// Plays the slide-down animation once, then leaves the panel painted.
void settingsScreenOnEnter();

void settingsScreenDraw();   // full repaint
void settingsScreenTick();   // input + incremental repaint (call each frame)

// Returns true exactly once after the user tapped the Wi-Fi row, so the
// caller can push the Wi-Fi management screen.
bool settingsScreenConsumeWifiTap();

// Returns true exactly once after the user tapped the top-right X button.
// Caller should close the Settings panel.
bool settingsScreenConsumeClose();
