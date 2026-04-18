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

using SayCallback = std::function<void(const String &userText)>;

// Connect to Wi-Fi (blocking, with short timeout). Returns true on success.
bool serverBeginWifi();

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
