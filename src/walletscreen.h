// ============================================================================
//  Wallet screen — slides up from the bottom when the user swipes UP on the
//  creature. The top bar carries a QR button (opens a full-screen QR of the
//  pubkey) and an X button (closes back to the creature). The body shows the
//  live address, SOL balance, USD value, and the full SPL token list with
//  USDC pinned first.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool walletScreenBegin(TFT_eSPI *tft);

// Called by main.cpp every time the wallet becomes the active screen. Plays
// the slide-up animation once, then leaves the wallet painted.
void walletScreenOnEnter();

void walletScreenDraw();     // full redraw (call on enter + when data refreshes)
void walletScreenTick();     // called ~60 FPS while this screen is active

// Forward the tap that touchPoll() already saw, so the wallet can hit-test
// QR / X / the dismiss-overlay target.
void walletScreenHandleTap(int16_t x, int16_t y);

// Latched "user asked to close the wallet" flag. Drained by main.cpp which
// then switches back to the creature screen.
bool walletScreenConsumeClose();
