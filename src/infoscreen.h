// ============================================================================
//  Info — firmware / chip / memory / network / wallet diagnostics. Reached
//  from the Menu screen's "Info" tile. Dynamic stats (heap, uptime, RSSI)
//  refresh once a second. X in the top-right returns home to the creature.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool infoScreenBegin(TFT_eSPI *tft);

// Called every time the info screen becomes active. Plays the slide-up
// animation once, then leaves the page painted.
void infoScreenOnEnter();

void infoScreenDraw();
void infoScreenTick();

// Forward a tap. Only the X button is hit-tested today.
void infoScreenHandleTap(int16_t x, int16_t y);

// Latched "user tapped X" intent, drained by main.cpp.
bool infoScreenConsumeClose();
