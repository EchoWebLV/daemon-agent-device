// ============================================================================
//  Wi-Fi + tiny HTTP server for talking to the Blue Gremlin.
//
//  The board has no onboard microphone, so instead of wiring one up we let
//  the user's phone do it: open the board's IP in a mobile Chrome browser
//  and tap a big talk button. The page uses the Web Speech API in the
//  browser to do the speech-to-text, then POSTs the transcribed text to
//  the board. The board calls Gemini + TTS and the creature replies out
//  loud while its mouth animates.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <functional>

using SayCallback       = std::function<void(const String &userText)>;
// Fires on every frame of the Wi-Fi connect wait loop (~30 Hz). Argument
// is milliseconds since `serverBeginWifi` was called. Use this to drive
// an on-screen progress/radar animation while the connect would
// otherwise block — the CPU is mostly idle during the wait, so spending
// a few ms/frame on UI is free.
using WifiJoinFrameCb   = std::function<void(uint32_t elapsedMs)>;

// Connect to Wi-Fi using NVS-stored credentials first, falling back to the
// compile-time defaults in secrets.h (blocking, 25 s timeout). Pass a
// frame callback to animate the connect wait; pass `nullptr` (or omit)
// to keep the legacy silent behaviour.
bool serverBeginWifi(WifiJoinFrameCb onFrame = nullptr);

// Attempt a connection to a specific SSID/password (blocking). On success
// the credentials are persisted in NVS so subsequent boots pick them up.
bool serverWifiConnect(const String &ssid, const String &password);

// Drop the current Wi-Fi association without forgetting the stored creds.
void serverWifiDisconnect();

// Start the HTTP listener; must be called after serverBeginWifi(). The
// provided callback fires on every incoming user utterance (from the phone).
void serverBeginHttp(SayCallback onSay);

// Must be called from loop() to service HTTP clients.
void serverLoop();

// Push a status line visible on the web page (Wi-Fi IP, "thinking…", etc).
void serverSetStatus(const String &s);

// Push the latest reply so the phone page can display + re-read it.
void serverSetReply(const String &user, const String &reply);

String serverLocalIP();
