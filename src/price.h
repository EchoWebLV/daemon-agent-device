// ============================================================================
//  SOL/USD price ticker. Fetched from CoinGecko (no key required) every
//  30 seconds. Rendered in the top-right of the status bar and injected
//  into the AI's context so Daemon can quote the current price.
// ============================================================================
#pragma once
#include <Arduino.h>

bool    priceBegin();
void    priceRefresh();         // blocking HTTPS fetch
void    priceRequestRefresh();  // fire-and-forget, runs on background task
double  priceSOLUSD();
String  priceDisplayString();
