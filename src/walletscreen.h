// ============================================================================
//  Wallet screen — second UI panel reachable by swiping LEFT from the
//  creature. Shows the live address, SOL balance, USD value, and a list of
//  SPL token holdings. Swipe RIGHT to go back to Daemon.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool walletScreenBegin(TFT_eSPI *tft);
void walletScreenDraw();     // full redraw (call on enter + when data refreshes)
void walletScreenTick();     // called ~60 FPS while this screen is active

// Re-render the entire wallet view into an off-screen sprite for the
// slide-transition module.
void walletScreenDrawTo(TFT_eSprite *target);
