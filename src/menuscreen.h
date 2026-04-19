// ============================================================================
//  Quick-actions menu — pull-up drawer reached by swiping UP from the
//  creature screen. Currently shows two big tappable tiles:
//    - Wallet  → opens the Phantom-style wallet drawer
//    - Info    → opens a device info panel (IP, firmware, uptime, …)
//  Swipe DOWN to dismiss back to the creature.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool menuScreenBegin(TFT_eSPI *tft);
void menuScreenDraw();   // full repaint (call on enter)
void menuScreenTick();   // called each frame while visible

// Edge-triggered: each of these returns true exactly once after the user
// taps the corresponding tile / close button. Main consumes the signal
// and pushes the matching screen.
bool menuScreenConsumeWalletTap();
bool menuScreenConsumeInfoTap();
bool menuScreenConsumeClose();
