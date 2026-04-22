// ============================================================================
//  Menu — the slide-up launcher reached by swiping UP from the creature.
//  A two-tile dashboard: one tile opens the wallet, the other opens the
//  firmware/diagnostics info screen. The X in the top-right returns home
//  to the creature (same rule as every other panel).
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool menuScreenBegin(TFT_eSPI *tft);

// Called every time the menu becomes the active screen. Plays the
// slide-up animation once, then leaves the tiles painted.
void menuScreenOnEnter();

void menuScreenDraw();   // full repaint (call on enter + when data refreshes)
void menuScreenTick();   // called ~60 FPS while this screen is active

// Forward a tap that touchPoll() already saw, so the menu can hit-test
// its buttons.
void menuScreenHandleTap(int16_t x, int16_t y);

// Latched "user tapped X" intent, drained by main.cpp. Returns to
// creature on consume.
bool menuScreenConsumeClose();

// Latched "user tapped Wallet tile" / "user tapped Info tile" intents.
// Main.cpp consumes whichever fired and switches to that screen.
bool menuScreenConsumeWalletTap();
bool menuScreenConsumeInfoTap();
