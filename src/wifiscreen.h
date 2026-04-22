// ============================================================================
//  Wi-Fi management panel. Sub-screen of Settings:
//    - Scan + display nearby networks (tap to select)
//    - Full on-screen QWERTY keyboard for WPA passwords
//    - Connect / Disconnect buttons
//    - Persists chosen credentials to NVS via devcfg/server
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

#include "touch.h"   // for SwipeDir

bool wifiScreenBegin(TFT_eSPI *tft);

// Called when entering the screen (fresh start).
void wifiScreenEnter();

// Called each frame while active.
void wifiScreenTick();

// Feed swipe events in. The Wi-Fi screen handles them contextually:
// swipes inside the keyboard / connecting / result modes go back to the
// network-picker list; swipes on the list itself exit the whole panel.
void wifiScreenHandleSwipe(SwipeDir sw);

// True if the user pressed "back" (or we're done). Main clears by calling
// `wifiScreenConsumeExit()`. Triggered by swipes and automatic transitions
// (e.g. successful connect) — routes the caller back to Settings.
bool wifiScreenConsumeExit();

// True exactly once after the user tapped the top-right X button. The
// semantic is "close the Wi-Fi panel and go all the way home to the
// creature screen" — distinct from wifiScreenConsumeExit() so main.cpp
// can route the two cases to different destinations.
bool wifiScreenConsumeCloseHome();
