// ============================================================================
//  Voice playback for the Blue Gremlin.
//
//  Wraps the ESP32-audioI2S library so the rest of the code doesn't need to
//  think about I2S directly. We expose:
//
//    voiceBegin()         — set up I2S pins + the audio task
//    voiceSpeak(text)     — synthesize via a free TTS endpoint and play
//    voiceIsSpeaking()    — true while audio is actively streaming
//    voiceStop()          — abort playback
//    voiceLoop()          — must be called frequently from loop()
//
//  I2S pin map (PCM5101 on the Waveshare 2.8"):
//      BCK  = GPIO48
//      LRCK = GPIO38
//      DIN  = GPIO47
// ============================================================================
#pragma once
#include <Arduino.h>

bool voiceBegin();
void voiceLoop();
bool voiceSpeak(const String &text);
bool voiceIsSpeaking();
void voiceStop();
