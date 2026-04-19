// ============================================================================
//  X posts screen — sub-screen reached by tapping "X" in the menu drawer.
//  Shows a scrollable list of the most recent tweets the auto-poster has
//  published this session (stored in the xpost module's RAM ring buffer).
//  Also shows a status line with enabled/disabled, interval, next-post ETA,
//  and last error if any. Swipe DOWN to return to the menu.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool xpostScreenBegin(TFT_eSPI *tft);
void xpostScreenDraw();   // full repaint (call on enter)
void xpostScreenTick();   // called each frame while visible

// True exactly once after the user taps the close "x" button.
bool xpostScreenConsumeClose();