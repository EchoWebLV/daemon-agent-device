// ============================================================================
//  Device info panel — sub-screen reached by tapping "Info" in the menu
//  drawer. Read-only view listing the device's IP, Wi-Fi details, MAC,
//  firmware build timestamp, SDK / chip info, memory usage, uptime and
//  wallet public key. Swipe DOWN to return to the menu.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool infoScreenBegin(TFT_eSPI *tft);
void infoScreenDraw();   // full repaint (call on enter)
void infoScreenTick();   // called each frame while visible

// Re-render the entire info panel into an off-screen sprite for the
// slide-transition module.
void infoScreenDrawTo(TFT_eSprite *target);

// True exactly once after the user taps the close button.
bool infoScreenConsumeClose();
