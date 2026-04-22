// ============================================================================
//  src/testharness.h
//
//  Host-driven test mode. When the laptop sends "TEST BEGIN\n" over the USB
//  CDC serial port, the firmware pauses its normal loop() body and responds
//  to a small verb set described in
//    docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md
//
//  Always compiled in. Costs almost nothing when idle — a single byte-level
//  Serial.available() poll per loop() iteration.
// ============================================================================
#pragma once
#include <Arduino.h>

// One-time setup. Reserves the line buffer. Safe to call before any screen
// modules are initialised.
void testHarnessBegin();

// Kept for backward-compatibility with older call sites. No-op: the
// single serial reader lives in main.cpp's pumpSerialInput(), which
// routes "TEST "-prefixed lines to testHarnessHandleLine() directly
// and passes everything else to the normal utterance pipeline. Having
// two readers race for bytes caused TEST lines to be swallowed as
// chat input, so we consolidated on one.
void testHarnessTick();

// Dispatch a single already-assembled line (without the trailing \n)
// that begins with "TEST ". Called by main.cpp's pumpSerialInput().
void testHarnessHandleLine(const String &line);

// True between "TEST BEGIN" and "TEST END". main.cpp's loop() should
// short-circuit its normal body while this is true so the host has full
// control of the device.
bool testHarnessInTestMode();
