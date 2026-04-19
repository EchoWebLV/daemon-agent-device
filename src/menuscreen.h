// ============================================================================
//  Quick-actions menu — pull-up drawer reached by swiping UP from the
//  creature screen. Shows three big tappable tiles:
//    - Wallet  → opens the Phantom-style wallet drawer
//    - X       → opens the recent-tweets list (auto-poster history)
//    - Info    → opens a device info panel (IP, firmware, uptime, …)
//  Swipe DOWN to dismiss back to the creature.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool menuScreenBegin(TFT_eSPI *tft);
void menuScreenDraw();   // full repaint (call on enter)
void menuScreenTick();   // called each frame while visible

// Re-renders the entire menu into the supplied off-screen sprite.
// Used by the slide-transition module to pre-render both sides of the
// animation. Internally this just temporarily binds the screen's
// painting target to the sprite, runs the existing draw, and restores.
void menuScreenDrawTo(TFT_eSprite *target);

// Edge-triggered: each of these returns true exactly once after the user
// taps the corresponding tile / close button. Main consumes the signal
// and pushes the matching screen.
bool menuScreenConsumeWalletTap();
bool menuScreenConsumeXTap();
bool menuScreenConsumeInfoTap();
bool menuScreenConsumeClose();
