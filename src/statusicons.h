// ============================================================================
//  statusicons — tiny glyphs for the top status bar.
//
//  Two indicators are drawn in the center of the bar when their respective
//  feature is enabled in devcfg:
//    • memory      — "stacked bars" icon (on-chain chat history is on)
//    • heartbeat   — filled heart (scheduled prompt loop is on)
//
//  Both icons glow in the Daemon accent colour. When a feature is off the
//  corresponding slot is blanked so toggles propagate immediately.
// ============================================================================
#pragma once
#include <TFT_eSPI.h>

// Draw both indicators centred on (centerX, centerY). `color` is the
// foreground (usually the Daemon accent blue); `bgColor` is used to
// wipe the area before drawing so the icons never ghost when the
// toggles flip.
void statusIconsDraw(TFT_eSPI *tft, int centerX, int centerY,
                     uint16_t color, uint16_t bgColor);

// True if either icon state differs from the last paint. Call after a
// successful `statusIconsDraw()` to reset the tracker, or manually via
// `statusIconsResetCache()` when the screen changes.
bool statusIconsNeedRedraw();
void statusIconsResetCache();
