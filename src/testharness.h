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

// Poll the USB serial port for one or more complete lines; if any start
// with "TEST ", dispatch them. Call every iteration of loop(). Returns
// immediately when nothing is buffered.
void testHarnessTick();

// True between "TEST BEGIN" and "TEST END". main.cpp's loop() should
// short-circuit its normal body while this is true so the host has full
// control of the device.
bool testHarnessInTestMode();
