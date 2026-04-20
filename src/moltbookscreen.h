// ============================================================================
//  Moltbook post history screen — shows recent posts to moltbook.com.
//  Reached from the Menu screen via the Moltbook tile.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

bool moltbookScreenBegin(TFT_eSPI *tft);
void moltbookScreenDraw();
void moltbookScreenTick();
void moltbookScreenDrawTo(TFT_eSprite *target);
bool moltbookScreenConsumeClose();
