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
// True whenever the voice subsystem is doing anything that holds TLS or
// LittleFS resources — the ElevenLabs fetch OR the MP3 playback. Other
// background tasks (Arweave upload, memory writes) should wait on this
// before opening their own TLS sessions to avoid OOM from concurrent
// mbedTLS contexts.
bool voiceIsBusy();
void voiceStop();
void voiceDiagnose();           // one-shot HTTP diag for debugging
void voiceSetVolume(uint8_t v); // 0..21, forwarded to the Audio library
