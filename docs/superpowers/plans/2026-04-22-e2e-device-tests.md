# E2E Device Smoke-Test Suite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver the E2E smoke-test suite described in `docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md` — one Python host runner + one firmware test-mode dispatcher + the ~15 lines of main.cpp glue that wires them together — so that running `python tests/run.py` against a USB-connected board exercises 16 named cases covering boot, UI, Wi-Fi, wallet, Gemini, and x402.

**Architecture:** Plain-text line protocol over the existing USB-CDC serial link. Firmware harness is always compiled in and idles until it sees `TEST BEGIN`; host runner is plain-Python (pyserial + requests + solana-py), single file, no test framework. Vertical slice first — stand up the BEGIN/END/PING round-trip end-to-end before growing sideways — so we never debug two unfinished sides at once.

**Tech Stack:** Arduino-ESP32 framework on PlatformIO (`[env:waveshare_esp32s3_28]`), Python 3.11+, `pyserial>=3.5`, `requests>=2.31`, `solana>=0.34`, `solders>=0.20`.

---

## Deviations from the spec (read first)

Two small adjustments surfaced while checking real API signatures. Both keep the spec's intent; they just match what `src/wallet.h` and `src/x402.h` already expose.

1. **Wallet balance is `double SOL`, not `uint64_t lamports`.** `walletSolBalance()` already returns a `double` (the UI amount). The `TEST WALLET BALANCE` verb therefore returns two floats — `<sol_float> <usdc_float>` — so the host can compare against `Client.get_balance / 1e9` with a small tolerance. The extra `usdc_float` avoids a second round-trip in `wallet_balance_matches` and feeds the x402 precondition check.

2. **`TEST X402 CALL <url>` really does take a URL.** `x402Post(url, jsonBody)` / `x402Get(url)` in `src/x402.h` are already generic, so no per-label refactor is needed. The host sends the concrete URL; the firmware POSTs an empty JSON body if the URL is unknown, or the correct body if it matches one of the known production endpoints we identify in Task 15.

These are implementation detail. If the user prefers we update the spec to reflect them verbatim, do that at the end.

## File structure

| Path                         | Action | Role                                                                                    |
| ---------------------------- | ------ | --------------------------------------------------------------------------------------- |
| `src/testharness.h`          | Create | Public API: `testHarnessBegin()`, `testHarnessTick()`, `testHarnessInTestMode()`        |
| `src/testharness.cpp`        | Create | Line reader + per-verb dispatcher (~300 LOC). Calls into wallet/x402/WiFi/screen APIs.  |
| `src/main.cpp`               | Modify | Add `testHarnessBegin()` in `setup()`, call `testHarnessTick()` at top of `loop()`, add swipe/tap injection helpers, add `mainScreenName()` getter. ~30 LOC net. |
| `tests/run.py`               | Create | Whole host runner: `Device`, `find_port`, `run_case`, 16 cases, CLI. ~250 LOC.           |
| `tests/requirements.txt`     | Create | `pyserial>=3.5`, `requests>=2.31`, `solana>=0.34`, `solders>=0.20`.                      |
| `tests/README.md`            | Create | One-pager: preconditions, `python tests/run.py`, sample output.                          |

No changes to `platformio.ini`.

---

## Task 1: Codebase discovery — confirm assumed APIs

**Why first:** the plan references function names (`x402Post`, `walletSolBalance`, `WiFi.scanNetworks`, `touchPoll`) whose signatures I've seen but haven't cross-referenced for every caller. Two minutes of confirmation now avoids thrashing later.

**Files:**
- Read-only: `src/main.cpp`, `src/wallet.h`, `src/x402.h`, `src/ai.h`, `src/server.cpp`

- [ ] **Step 1:** Confirm exactly how swipes dispatch in `main.cpp::loop()`. Goal: find the function or branch that reacts to a `SwipeDir` value, so Task 6 can expose it to the harness without duplication.

```bash
grep -n "touchPoll\|SwipeDir\|sw ==" src/main.cpp | head -40
```

Expected: a block around line 260–300 with `SwipeDir sw = touchPoll();` followed by per-screen `if (sw == SWIPE_DOWN) switchScreen(...)`. Note the branch boundaries — Task 6 will move it into `mainDispatchSwipe(SwipeDir)`.

- [ ] **Step 2:** Confirm how taps are delivered to screens in `main.cpp`.

```bash
grep -n "touchJustPressed\|HandleTap" src/main.cpp | head -20
```

Expected: a block using `touchJustPressed(x, y)` that calls the current screen's `*HandleTap(x, y)`. Note the dispatch shape — Task 7 will expose it as `mainInjectTap(x, y)`.

- [ ] **Step 3:** Locate the three x402 production endpoints. Goal: have real URLs ready for Task 15.

```bash
grep -n "x402Post\|x402Get" src/*.cpp | head -40
```

Expected: call sites in `ai.cpp` (Gemini), `server.cpp` or `services.cpp` (service catalog), and possibly one other. Write the three URLs (or the URL-building expressions) into a scratch note for Task 15; you'll paste them into `tests/run.py`'s `X402_URLS` list there.

- [ ] **Step 4:** Confirm `ai.h` has a one-shot prompt entry point the harness can reuse for `TEST AI PING`.

```bash
grep -n "aiAsk\|aiPrompt\|aiChat\|^.*Gemini\|String.*prompt" src/ai.h
```

Expected: a single public function that takes a prompt string, makes the call, and returns a response/status. Record its name for Task 14.

- [ ] **Step 5:** No commit (nothing changed). Move on.

---

## Task 2: `src/testharness.h` — public API

**Files:**
- Create: `src/testharness.h`

- [ ] **Step 1:** Write the header.

```cpp
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
```

- [ ] **Step 2:** Compile to confirm no syntax error.

```bash
pio run -e waveshare_esp32s3_28
```

Expected: succeeds (header isn't included anywhere yet, but the build should still finish).

- [ ] **Step 3:** Commit.

```bash
git add src/testharness.h
git commit -m "test-harness: public API for host-driven smoke tests"
```

---

## Task 3: `src/testharness.cpp` — skeleton + BEGIN/END/PING

**Goal:** get the line reader working with three verbs. Everything else stacks on top in later tasks.

**Files:**
- Create: `src/testharness.cpp`

- [ ] **Step 1:** Write the skeleton.

```cpp
// ============================================================================
//  src/testharness.cpp — see testharness.h header for protocol overview.
// ============================================================================
#include "testharness.h"

static bool   s_testMode = false;
static String s_lineBuf;

// Forward decl; defined below, grows as we add verbs.
static void handleLine(const String &line);

void testHarnessBegin() {
  s_lineBuf.reserve(256);
}

bool testHarnessInTestMode() {
  return s_testMode;
}

void testHarnessTick() {
  // Non-blocking line assembly. Any byte that isn't \r/\n appends to the
  // buffer; \n flushes the buffer through the dispatcher.
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      if (s_lineBuf.length() > 0) {
        handleLine(s_lineBuf);
      }
      s_lineBuf = "";
    } else if (s_lineBuf.length() < 255) {
      s_lineBuf += (char)c;
    }
    // If the buffer would overflow, the extra bytes are silently dropped.
    // The next \n still flushes whatever was captured, so we never wedge.
  }
}

// ---------------------------------------------------------------------------
// Dispatcher. Each verb is one `if (rest.startsWith("..."))` branch.
// Unknown verbs must respond, not hang, so the host's readline() never
// times out on a typo.
// ---------------------------------------------------------------------------
static void handleLine(const String &line) {
  if (!line.startsWith("TEST ")) return;   // not our traffic, ignore
  const String rest = line.substring(5);   // drop the "TEST " prefix

  // --- BEGIN / END / PING ------------------------------------------------
  if (rest == "BEGIN") {
    s_testMode = true;
    Serial.println("TEST OK begin");
    return;
  }
  if (rest == "END") {
    s_testMode = false;
    Serial.println("TEST OK end");
    return;
  }
  if (rest == "PING") {
    Serial.printf("TEST OK ping %u\n", (unsigned)millis());
    return;
  }

  // --- Unknown verb: always respond -------------------------------------
  Serial.printf("TEST ERR unknown %s\n", rest.c_str());
}
```

- [ ] **Step 2:** Compile.

```bash
pio run -e waveshare_esp32s3_28
```

Expected: succeeds.

- [ ] **Step 3:** Commit.

```bash
git add src/testharness.cpp
git commit -m "test-harness: line reader + BEGIN/END/PING verbs"
```

---

## Task 4: Wire `testHarnessTick()` into `main.cpp`

**Files:**
- Modify: `src/main.cpp` — add include, call `testHarnessBegin()` in `setup()`, call `testHarnessTick()` + early-return from `loop()`.

- [ ] **Step 1:** Add the include near the other screen includes at the top of `main.cpp`.

Find the block like:
```cpp
#include "menuscreen.h"
#include "walletscreen.h"
```
and add:
```cpp
#include "testharness.h"
```

- [ ] **Step 2:** Add `testHarnessBegin()` to `setup()`. Place it near the end of `setup()` alongside the other `*Begin()` calls.

```cpp
  // ... existing *Begin calls ...
  testHarnessBegin();
```

- [ ] **Step 3:** At the very top of `loop()`, before anything else, add the harness tick and the short-circuit.

```cpp
void loop() {
  // Host-driven smoke tests preempt the normal UI loop while active.
  testHarnessTick();
  if (testHarnessInTestMode()) {
    delay(1);          // yield to Wi-Fi / background tasks
    return;
  }

  // ... existing loop body unchanged ...
}
```

- [ ] **Step 4:** Compile.

```bash
pio run -e waveshare_esp32s3_28
```

Expected: succeeds.

- [ ] **Step 5:** Flash the board and open the serial monitor briefly to confirm the device still boots normally (no test commands sent yet — harness is dormant).

```bash
pio run -e waveshare_esp32s3_28 -t upload
pio device monitor -b 115200
```

Expected: normal boot banner from `Serial.printf`; creature appears on the screen. Press Ctrl-C to exit the monitor.

- [ ] **Step 6:** Commit.

```bash
git add src/main.cpp
git commit -m "test-harness: wire testHarnessTick into main loop"
```

---

## Task 5: `tests/requirements.txt` and `tests/README.md`

**Files:**
- Create: `tests/requirements.txt`
- Create: `tests/README.md`

- [ ] **Step 1:** Create `tests/requirements.txt`.

```
pyserial>=3.5
requests>=2.31
solana>=0.34
solders>=0.20
```

- [ ] **Step 2:** Create `tests/README.md`.

```markdown
# Device E2E tests

Smoke tests that exercise the Daemon board over USB. One Python driver,
one firmware harness. No framework.

## Preconditions

- Device flashed with current `main` and connected via USB.
- Device has previously joined a Wi-Fi network (credentials in NVS).
- Wallet holds ≥ 0.001 SOL and ≥ $0.05 USDC on Solana mainnet
  (x402 cases skip themselves if short).
- Laptop can reach the public internet.

## Install

```bash
pip install -r tests/requirements.txt
```

## Run

```bash
# Full run — 16 cases, ~45 s
python tests/run.py

# Fast iteration — skip the 3 paid cases, ~10 s
python tests/run.py --skip-x402

# Filter by substring
python tests/run.py --only wifi
python tests/run.py --only screen_roundtrip

# Override USB port auto-detect
python tests/run.py --port /dev/cu.usbmodem2101

# Use a private Solana RPC
python tests/run.py --rpc https://mainnet.helius-rpc.com/?api-key=...
```

## Exit codes

- `0` — all selected cases passed
- `1` — at least one case failed
- `2` — device not found / ambiguous port / can't open serial
```

- [ ] **Step 3:** Commit.

```bash
git add tests/requirements.txt tests/README.md
git commit -m "tests: add requirements.txt and README"
```

---

## Task 6: `tests/run.py` — scaffolding + `c_liveness` case

**Goal:** stand up the host runner end-to-end with exactly one case that sends `TEST PING` and asserts a response. Everything afterwards is just another case on top of this skeleton.

**Files:**
- Create: `tests/run.py`

- [ ] **Step 1:** Write the scaffolding and the one case.

```python
#!/usr/bin/env python3
"""
tests/run.py — host driver for the on-device smoke-test harness.

See docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md for the
protocol and test-case contract. Everything the runner needs lives in this
single file on purpose: readable top-to-bottom, no test framework.
"""

import argparse
import sys
import time
from typing import Callable, List, Optional

import serial
import serial.tools.list_ports

# --- Config ----------------------------------------------------------------

BAUD = 115200
DEFAULT_RPC = "https://api.mainnet-beta.solana.com"

# Filled in Task 15 from `grep x402Post src/*.cpp`. Keep empty until then.
X402_URLS: list[tuple[str, str]] = [
    # ("quote",   "https://..."),
    # ("image",   "https://..."),
    # ("catalog", "https://..."),
]


# --- Device wrapper --------------------------------------------------------

class Device:
    """Thin wrapper around pyserial that sends one TEST command and collects
    lines until an OK/ERR response. Multi-line responses (WIFI SCAN) are
    handled by peeking at the `<n>` count and reading exactly N tails."""

    def __init__(self, port: str):
        self.ser = serial.Serial(port, BAUD, timeout=0.1)
        time.sleep(0.5)                 # let CDC stabilize after open
        self.ser.reset_input_buffer()
        self.trail: list[str] = []       # non-TEST Serial chatter, for failure logs

    def send(self, line: str, timeout: float = 2.0) -> list[str]:
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            s = raw.decode(errors="replace").rstrip("\r\n")
            if s.startswith("TEST OK") or s.startswith("TEST ERR"):
                tokens = s.split()
                # WIFI SCAN is the one multi-line response. Format:
                #   TEST OK wifi scan <n>
                #   TEST NET <ssid> <rssi> <enc>   (×n)
                if (len(tokens) >= 5 and tokens[1] == "OK"
                        and tokens[2] == "wifi" and tokens[3] == "scan"):
                    n = int(tokens[4])
                    tails = []
                    for _ in range(n):
                        cont = self.ser.readline().decode(errors="replace").rstrip("\r\n")
                        tails.append(cont)
                    return [s] + tails
                return [s]
            if s:
                self.trail.append(s)
        raise TimeoutError(
            f"no response to {line!r} in {timeout}s; recent Serial: "
            f"{self.trail[-5:]}")

    def begin(self):  self.send("TEST BEGIN")
    def end(self):    self.send("TEST END")


# --- Port discovery --------------------------------------------------------

def find_port() -> Optional[str]:
    """Return the path of the CDC serial port the ESP32-S3 enumerates as,
    or None if zero/ambiguous matches. Matches on vendor/product strings
    seen from Waveshare ESP32-S3 boards ('USB JTAG/serial debug unit')."""
    ports = list(serial.tools.list_ports.comports())
    candidates = [
        p for p in ports
        if (p.vid, p.pid) == (0x303a, 0x1001)      # ESP32-S3 built-in USB
           or "USB JTAG" in (p.description or "")
           or "CP210" in (p.description or "")
    ]
    if len(candidates) == 1:
        return candidates[0].device
    if len(candidates) > 1:
        names = ", ".join(c.device for c in candidates)
        print(f"multiple candidate ports: {names}; pass --port", file=sys.stderr)
    return None


# --- Case runner -----------------------------------------------------------

def run_case(dev: Device, name: str, fn: Callable[[Device], str]) -> bool:
    """Run one case. The case function returns a one-line status string on
    PASS or raises to FAIL. Prints a formatted line either way."""
    label = f"[PASS]"
    try:
        detail = fn(dev) or ""
    except AssertionError as e:
        print(f"[FAIL] {name:<26}  {e}")
        for line in dev.trail[-10:]:
            print(f"       | {line}")
        dev.trail.clear()
        return False
    except Exception as e:
        print(f"[FAIL] {name:<26}  {type(e).__name__}: {e}")
        for line in dev.trail[-10:]:
            print(f"       | {line}")
        dev.trail.clear()
        return False
    print(f"{label} {name:<26}  {detail}")
    return True


# --- Cases -----------------------------------------------------------------

def c_liveness(dev: Device) -> str:
    """Round-trip a single PING and extract the uptime report."""
    resp = dev.send("TEST PING", timeout=2.0)
    assert resp and resp[0].startswith("TEST OK ping "), f"bad response: {resp}"
    uptime_ms = int(resp[0].split()[-1])
    return f"(uptime {uptime_ms} ms)"


CASES: list[tuple[str, Callable[[Device], str]]] = [
    ("liveness", c_liveness),
    # more cases added in Tasks 7, 10, 11, 12, 13, 14, 16, 17
]


# --- Main ------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description="Daemon device E2E smoke tests")
    ap.add_argument("--port",      default=None, help="override USB port auto-detect")
    ap.add_argument("--rpc",       default=DEFAULT_RPC, help="Solana RPC URL")
    ap.add_argument("--only",      default=None, help="case-name substring filter")
    ap.add_argument("--skip-x402", action="store_true", help="skip paid cases")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("no device found — is the board plugged in?", file=sys.stderr)
        return 2

    print(f"device: {port} @ {BAUD} baud")

    try:
        dev = Device(port)
    except serial.SerialException as e:
        print(f"could not open {port}: {e}", file=sys.stderr)
        return 2

    dev.begin()
    print("-> TEST BEGIN\n")
    try:
        passed = failed = skipped = 0
        for name, fn in CASES:
            if args.only and args.only not in name:
                continue
            if args.skip_x402 and name.startswith("x402_"):
                print(f"[SKIP] {name:<26}  (--skip-x402)")
                skipped += 1
                continue
            ok = run_case(dev, name, fn)
            passed += int(ok); failed += int(not ok)
        print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
        return 0 if failed == 0 else 1
    finally:
        dev.end()
        print("<- TEST END")


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2:** Install the Python deps.

```bash
pip install -r tests/requirements.txt
```

Expected: pyserial, requests, solana, solders all install cleanly.

- [ ] **Step 3:** Run against the (already flashed) device.

```bash
python tests/run.py --only liveness
```

Expected:
```
device: /dev/cu.usbmodem101 @ 115200 baud
-> TEST BEGIN

[PASS] liveness                   (uptime 12345 ms)

1 passed, 0 failed, 0 skipped
<- TEST END
```

- [ ] **Step 4:** Commit.

```bash
git add tests/run.py
git commit -m "tests: host runner scaffolding + liveness case"
```

---

## Task 7: HEAP + VERSION verbs and cases

**Files:**
- Modify: `src/testharness.cpp` — add `HEAP` and `VERSION` branches above the unknown-verb fallback.
- Modify: `tests/run.py` — add `c_boot_heap_ok`, `c_version_reports` cases.

- [ ] **Step 1:** In `src/testharness.cpp`, add the two verb branches before the `TEST ERR unknown` fallback.

```cpp
  if (rest == "HEAP") {
    Serial.printf("TEST OK heap %u %u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram());
    return;
  }
  if (rest == "VERSION") {
    // SDK + build timestamp. Keep build_date / build_time in a single
    // token each — the protocol is space-delimited.
    String buildDate = __DATE__;  buildDate.replace(' ', '_');
    String buildTime = __TIME__;
    Serial.printf("TEST OK version %s %s %s\n",
                  ESP.getSdkVersion(),
                  buildDate.c_str(),
                  buildTime.c_str());
    return;
  }
```

- [ ] **Step 2:** Compile + flash.

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 3:** In `tests/run.py`, add the two case functions above the `CASES = [` line.

```python
def c_boot_heap_ok(dev: Device) -> str:
    resp = dev.send("TEST HEAP", timeout=2.0)
    toks = resp[0].split()
    assert toks[:3] == ["TEST", "OK", "heap"], f"bad response: {resp}"
    heap  = int(toks[3])
    psram = int(toks[4])
    # Post-boot we should have plenty. 80 KB heap is a generous floor;
    # an audio buffer alloc leaves us well above this.
    assert heap  > 80 * 1024,        f"free heap too low: {heap} B"
    assert psram > 4 * 1024 * 1024,  f"free psram too low: {psram} B"
    return f"(heap {heap // 1024} KB, psram {psram // (1024*1024)} MB)"

def c_version_reports(dev: Device) -> str:
    resp = dev.send("TEST VERSION", timeout=2.0)
    toks = resp[0].split()
    assert toks[:3] == ["TEST", "OK", "version"], f"bad response: {resp}"
    sdk, date, time_s = toks[3], toks[4], toks[5]
    assert sdk,  "empty sdk"
    assert date, "empty build date"
    assert time_s, "empty build time"
    return f"({sdk}, built {date} {time_s})"
```

- [ ] **Step 4:** Add them to `CASES` (just after `("liveness", c_liveness),`).

```python
    ("boot_heap_ok",    c_boot_heap_ok),
    ("version_reports", c_version_reports),
```

- [ ] **Step 5:** Run.

```bash
python tests/run.py --only boot_heap_ok
python tests/run.py --only version_reports
```

Expected: both PASS.

- [ ] **Step 6:** Commit.

```bash
git add src/testharness.cpp tests/run.py
git commit -m "test-harness: HEAP + VERSION verbs with cases"
```

---

## Task 8: Screen name helpers in `main.cpp`

**Why:** the harness needs to both *read* and *set* the current screen. `main.cpp` owns the `Screen` enum and `switchScreen()`. Add a name↔enum bridge so `testharness.cpp` stays UI-agnostic.

**Files:**
- Modify: `src/main.cpp` — add two free functions.
- Modify: `src/testharness.cpp` — forward-declare them near the top.

- [ ] **Step 1:** In `src/main.cpp`, just below the `switchScreen()` function, add:

```cpp
// ---------------------------------------------------------------------------
// Test-harness bridges. Exposed as extern-C-style free functions so the
// harness module (src/testharness.cpp) doesn't need to know about the
// Screen enum. Names match the on-protocol casing so the mapping is 1:1.
// ---------------------------------------------------------------------------
const char *mainScreenName() {
  switch (s_screen) {
    case SCREEN_CREATURE: return "creature";
    case SCREEN_MENU:     return "menu";
    case SCREEN_WALLET:   return "wallet";
    case SCREEN_INFO:     return "info";
    case SCREEN_SETTINGS: return "settings";
    case SCREEN_WIFI:     return "wifi";
  }
  return "unknown";
}

// Returns true if `name` matched a known screen; false otherwise.
bool mainForceScreen(const char *name) {
  if      (!strcmp(name, "creature")) switchScreen(SCREEN_CREATURE);
  else if (!strcmp(name, "menu"))     switchScreen(SCREEN_MENU);
  else if (!strcmp(name, "wallet"))   switchScreen(SCREEN_WALLET);
  else if (!strcmp(name, "info"))     switchScreen(SCREEN_INFO);
  else if (!strcmp(name, "settings")) switchScreen(SCREEN_SETTINGS);
  else if (!strcmp(name, "wifi"))     switchScreen(SCREEN_WIFI);
  else return false;
  return true;
}
```

- [ ] **Step 2:** In `src/testharness.cpp`, add forward declarations at file scope, below the existing `static` declarations.

```cpp
// Implemented in main.cpp.
extern const char *mainScreenName();
extern bool        mainForceScreen(const char *name);
```

- [ ] **Step 3:** Compile.

```bash
pio run -e waveshare_esp32s3_28
```

Expected: succeeds.

- [ ] **Step 4:** Commit.

```bash
git add src/main.cpp src/testharness.cpp
git commit -m "main: expose screen-name bridge for test harness"
```

---

## Task 9: SCREEN GET / FORCE / PAINT verbs and cases

**Files:**
- Modify: `src/testharness.cpp`
- Modify: `tests/run.py`

- [ ] **Step 1:** In `src/testharness.cpp`, add the three verbs before the unknown-verb fallback.

```cpp
  if (rest == "SCREEN GET") {
    Serial.printf("TEST OK screen %s\n", mainScreenName());
    return;
  }
  if (rest.startsWith("SCREEN FORCE ")) {
    String name = rest.substring(strlen("SCREEN FORCE "));
    if (!mainForceScreen(name.c_str())) {
      Serial.printf("TEST ERR screen unknown %s\n", name.c_str());
      return;
    }
    Serial.printf("TEST OK screen %s\n", mainScreenName());
    return;
  }
  if (rest == "SCREEN PAINT") {
    uint32_t t0 = millis();
    // Re-enter the current screen to force a full repaint of its body.
    // Avoids the ~220 ms slide-in: we call the Draw() path directly.
    if      (!strcmp(mainScreenName(), "creature")) creatureRepaint();
    else if (!strcmp(mainScreenName(), "menu"))     menuScreenDraw();
    else if (!strcmp(mainScreenName(), "wallet"))   walletScreenDraw();
    else if (!strcmp(mainScreenName(), "info"))     infoScreenDraw();
    else if (!strcmp(mainScreenName(), "settings")) settingsScreenDraw();
    else if (!strcmp(mainScreenName(), "wifi"))     wifiScreenDraw();
    Serial.printf("TEST OK paint %u\n", (unsigned)(millis() - t0));
    return;
  }
```

- [ ] **Step 2:** Add the screen-module includes at the top of `src/testharness.cpp`.

```cpp
#include "creature.h"
#include "menuscreen.h"
#include "walletscreen.h"
#include "infoscreen.h"
#include "settingsscreen.h"
#include "wifiscreen.h"
```

Note: adjust names if Task 1's discovery showed different entry points (e.g. `settingsScreenRepaint` instead of `settingsScreenDraw`). Match real headers.

- [ ] **Step 3:** Flash.

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 4:** In `tests/run.py`, add the cases.

```python
SCREENS = ["creature", "menu", "wallet", "info", "settings", "wifi"]

def c_screen_roundtrip(dev: Device) -> str:
    for name in SCREENS:
        resp = dev.send(f"TEST SCREEN FORCE {name}", timeout=3.0)
        assert resp[0].endswith(name), f"force {name} returned {resp}"
        # Confirm via a separate GET so we're not trusting the FORCE reply.
        resp = dev.send("TEST SCREEN GET", timeout=2.0)
        got = resp[0].split()[-1]
        assert got == name, f"GET after FORCE {name} returned {got}"
    return f"({len(SCREENS)}/{len(SCREENS)} screens)"

def c_paint_under_budget(dev: Device) -> str:
    worst = 0
    for name in SCREENS:
        dev.send(f"TEST SCREEN FORCE {name}", timeout=3.0)
        time.sleep(0.25)  # let slide-in settle before measuring paint
        resp = dev.send("TEST SCREEN PAINT", timeout=3.0)
        ms = int(resp[0].split()[-1])
        assert ms < 60, f"{name} repaint took {ms} ms (budget 60)"
        if ms > worst: worst = ms
    return f"(max {worst} ms)"
```

- [ ] **Step 5:** Add them to `CASES`.

```python
    ("screen_roundtrip",   c_screen_roundtrip),
    ("paint_under_budget", c_paint_under_budget),
```

- [ ] **Step 6:** Run.

```bash
python tests/run.py --only screen_
```

Expected: both PASS. Note — the display will visibly cycle through all six screens during the run. That's the test doing its job.

- [ ] **Step 7:** Commit.

```bash
git add src/testharness.cpp tests/run.py
git commit -m "test-harness: SCREEN GET/FORCE/PAINT verbs with cases"
```

---

## Task 10: Refactor swipe dispatch + add `mainInjectTap`

**Why:** `main.cpp::loop()` currently has swipe handling inline, which the harness can't reach. Hoist it into a free function so both the normal loop and `TEST SWIPE` can use the same dispatch logic — no duplication.

**Files:**
- Modify: `src/main.cpp` — extract swipe branch, add `mainInjectTap` and `mainInjectSwipe`.
- Modify: `src/testharness.cpp` — forward-declare the two injectors.

- [ ] **Step 1:** In `src/main.cpp`, find the swipe handling in `loop()`. It looks like:

```cpp
  SwipeDir sw = touchPoll();
  if (s_screen == SCREEN_SETTINGS) {
    // ... settings-specific branches ...
  } else if (s_screen == SCREEN_WIFI) {
    wifiScreenHandleSwipe(sw);
  }
  // ... etc ...
```

Cut this entire block (starting from `if (s_screen ==`) into a new free function placed just below `switchScreen()`:

```cpp
// ---------------------------------------------------------------------------
// Dispatch one swipe gesture to whichever screen is currently active.
// Extracted from loop() so the test harness can inject synthetic swipes
// via mainInjectSwipe() without duplicating the big per-screen branch.
// ---------------------------------------------------------------------------
static void dispatchSwipe(SwipeDir sw) {
  if (sw == SWIPE_NONE) return;

  if (s_screen == SCREEN_SETTINGS) {
    // ... (paste the original settings branch here) ...
  } else if (s_screen == SCREEN_WIFI) {
    wifiScreenHandleSwipe(sw);
  }
  // ... (paste the rest exactly as it was) ...
}
```

Back in `loop()`, replace what you cut with:

```cpp
  SwipeDir sw = touchPoll();
  dispatchSwipe(sw);
```

Compile to confirm no syntax error:

```bash
pio run -e waveshare_esp32s3_28
```

Expected: succeeds.

- [ ] **Step 2:** Add the two injector helpers below `dispatchSwipe`.

```cpp
// Inject a synthetic swipe as if the touchscreen had produced it. Used
// by src/testharness.cpp for TEST SWIPE.
void mainInjectSwipe(int dir) {
  dispatchSwipe((SwipeDir)dir);
}

// Inject a synthetic tap at (x, y) into the currently active screen.
// Mirrors the dispatch that `loop()` already does for real taps from
// `touchJustPressed`, but reading the current screen straight from
// s_screen instead of going through the touch driver.
void mainInjectTap(int16_t x, int16_t y) {
  switch (s_screen) {
    case SCREEN_CREATURE: /* creature ignores taps today */      break;
    case SCREEN_MENU:     menuScreenHandleTap(x, y);             break;
    case SCREEN_WALLET:   walletScreenHandleTap(x, y);           break;
    case SCREEN_INFO:     infoScreenHandleTap(x, y);             break;
    case SCREEN_SETTINGS: settingsScreenHandleTap(x, y);         break;
    case SCREEN_WIFI:     wifiScreenHandleTap(x, y);             break;
  }
}
```

If `creatureRepaint()` has a tap handler, add that too; if not, leave the comment.

- [ ] **Step 3:** In `src/testharness.cpp`, forward-declare them at file scope alongside the others.

```cpp
extern void mainInjectTap(int16_t x, int16_t y);
extern void mainInjectSwipe(int dir);
```

- [ ] **Step 4:** Compile.

```bash
pio run -e waveshare_esp32s3_28
```

Expected: succeeds.

- [ ] **Step 5:** Flash + sanity-check normal gestures still work.

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

Swipe up on the creature, confirm the menu opens. Swipe down, confirm settings. The refactor should be invisible to the user.

- [ ] **Step 6:** Commit.

```bash
git add src/main.cpp src/testharness.cpp
git commit -m "main: extract dispatchSwipe + add tap/swipe injectors for tests"
```

---

## Task 11: TAP + SWIPE verbs and three UI cases

**Files:**
- Modify: `src/testharness.cpp`
- Modify: `tests/run.py`

- [ ] **Step 1:** In `src/testharness.cpp`, add the two verbs.

```cpp
  if (rest.startsWith("TAP ")) {
    int x = 0, y = 0;
    if (sscanf(rest.c_str(), "TAP %d %d", &x, &y) == 2) {
      mainInjectTap((int16_t)x, (int16_t)y);
      Serial.println("TEST OK tap");
    } else {
      Serial.println("TEST ERR tap bad_args");
    }
    return;
  }
  if (rest.startsWith("SWIPE ")) {
    String dir = rest.substring(6);
    int d = 0;
    if      (dir == "LEFT")  d = SWIPE_LEFT;
    else if (dir == "RIGHT") d = SWIPE_RIGHT;
    else if (dir == "UP")    d = SWIPE_UP;
    else if (dir == "DOWN")  d = SWIPE_DOWN;
    else {
      Serial.printf("TEST ERR swipe bad_dir %s\n", dir.c_str());
      return;
    }
    mainInjectSwipe(d);
    Serial.println("TEST OK swipe");
    return;
  }
```

Include `touch.h` at the top of `testharness.cpp` for the `SWIPE_*` enum values.

```cpp
#include "touch.h"
```

- [ ] **Step 2:** Flash.

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 3:** In `tests/run.py`, add the three UI cases. Tile centers are from `src/menuscreen.cpp` (TILE_X=10, TILE_W=220, TILE1_Y=44, TILE_H=116, TILE2_Y=172).

```python
# Menu tile centers, derived from src/menuscreen.cpp constants.
_MENU_TILE_X = 10 + 220 // 2                   # TILE_X + TILE_W/2 = 120
_WALLET_TILE_Y = 44 + 116 // 2                 # TILE1_Y + TILE_H/2 = 102
_INFO_TILE_Y   = 172 + 116 // 2                # TILE2_Y + TILE_H/2 = 230

def c_menu_tap_wallet(dev: Device) -> str:
    dev.send("TEST SCREEN FORCE menu", timeout=3.0)
    time.sleep(0.3)
    dev.send(f"TEST TAP {_MENU_TILE_X} {_WALLET_TILE_Y}", timeout=2.0)
    time.sleep(0.3)
    got = dev.send("TEST SCREEN GET", timeout=2.0)[0].split()[-1]
    assert got == "wallet", f"expected wallet, got {got}"
    return ""

def c_menu_tap_info(dev: Device) -> str:
    dev.send("TEST SCREEN FORCE menu", timeout=3.0)
    time.sleep(0.3)
    dev.send(f"TEST TAP {_MENU_TILE_X} {_INFO_TILE_Y}", timeout=2.0)
    time.sleep(0.3)
    got = dev.send("TEST SCREEN GET", timeout=2.0)[0].split()[-1]
    assert got == "info", f"expected info, got {got}"
    return ""

def c_settings_from_swipe(dev: Device) -> str:
    dev.send("TEST SCREEN FORCE creature", timeout=3.0)
    time.sleep(0.3)
    dev.send("TEST SWIPE DOWN", timeout=2.0)
    time.sleep(0.3)
    got = dev.send("TEST SCREEN GET", timeout=2.0)[0].split()[-1]
    assert got == "settings", f"expected settings, got {got}"
    return ""
```

- [ ] **Step 4:** Add to `CASES`.

```python
    ("menu_tap_wallet",     c_menu_tap_wallet),
    ("menu_tap_info",       c_menu_tap_info),
    ("settings_from_swipe", c_settings_from_swipe),
```

- [ ] **Step 5:** Run.

```bash
python tests/run.py --only menu_tap
python tests/run.py --only settings_from_swipe
```

Expected: all PASS.

- [ ] **Step 6:** Commit.

```bash
git add src/testharness.cpp tests/run.py
git commit -m "test-harness: TAP + SWIPE verbs with 3 UI cases"
```

---

## Task 12: WIFI STATUS + SCAN verbs and cases

**Files:**
- Modify: `src/testharness.cpp`
- Modify: `tests/run.py`

- [ ] **Step 1:** In `src/testharness.cpp`, add the two verbs. Include `<WiFi.h>` at the top.

```cpp
#include <WiFi.h>
```

```cpp
  if (rest == "WIFI STATUS") {
    if (WiFi.status() == WL_CONNECTED) {
      // SSIDs may contain spaces. Replace with underscores so the host's
      // space-delimited parser sees a single token. The host reverses it.
      String ssid = WiFi.SSID();
      ssid.replace(' ', '_');
      Serial.printf("TEST OK wifi connected %s %d\n",
                    ssid.c_str(), (int)WiFi.RSSI());
    } else {
      Serial.printf("TEST OK wifi disconnected - 0\n");
    }
    return;
  }
  if (rest == "WIFI SCAN") {
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false, true);   // sync, include hidden
    if (n < 0) {
      Serial.printf("TEST ERR wifi scan rc=%d\n", n);
      return;
    }
    Serial.printf("TEST OK wifi scan %d\n", n);
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i); ssid.replace(' ', '_');
      const char *enc = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "WPA";
      Serial.printf("TEST NET %s %d %s\n",
                    ssid.c_str(), (int)WiFi.RSSI(i), enc);
    }
    WiFi.scanDelete();
    return;
  }
```

- [ ] **Step 2:** Flash.

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 3:** In `tests/run.py`, add the two cases.

```python
def c_wifi_status_connected(dev: Device) -> str:
    resp = dev.send("TEST WIFI STATUS", timeout=3.0)[0].split()
    # Format: TEST OK wifi <connected|disconnected> <ssid_or_-> <rssi>
    assert resp[3] == "connected", f"wifi is {resp[3]}"
    ssid = resp[4].replace("_", " ")
    rssi = int(resp[5])
    assert ssid, "empty ssid"
    assert -100 < rssi < 0, f"implausible rssi {rssi}"
    return f"({ssid}, {rssi} dBm)"

def c_wifi_scan_nonempty(dev: Device) -> str:
    # Sync scan takes 2–4 s on ESP32. Give it 10.
    resp = dev.send("TEST WIFI SCAN", timeout=10.0)
    head = resp[0].split()
    n = int(head[-1])
    assert n >= 1, "scan returned 0 networks (regression!)"
    ssids = [ln.split()[2].replace("_", " ") for ln in resp[1:]]
    return f"({n} networks: {', '.join(ssids[:3])}{'…' if n > 3 else ''})"
```

- [ ] **Step 4:** Add to `CASES`.

```python
    ("wifi_status_connected", c_wifi_status_connected),
    ("wifi_scan_nonempty",    c_wifi_scan_nonempty),
```

- [ ] **Step 5:** Run.

```bash
python tests/run.py --only wifi
```

Expected: both PASS. The scan takes ~3 s.

- [ ] **Step 6:** Commit.

```bash
git add src/testharness.cpp tests/run.py
git commit -m "test-harness: WIFI STATUS + SCAN verbs with cases"
```

---

## Task 13: WALLET PUBKEY + BALANCE verbs and cases

**Files:**
- Modify: `src/testharness.cpp`
- Modify: `tests/run.py`

- [ ] **Step 1:** In `src/testharness.cpp`, add the two verbs. Include `wallet.h` at the top.

```cpp
#include "wallet.h"
```

```cpp
  if (rest == "WALLET PUBKEY") {
    const String pk = walletPubkey();
    if (pk.length() == 0) {
      Serial.println("TEST ERR wallet no_pubkey");
    } else {
      Serial.printf("TEST OK pubkey %s\n", pk.c_str());
    }
    return;
  }
  if (rest == "WALLET BALANCE") {
    // Force a fresh refresh so the host isn't reading a minute-old cache.
    walletRefresh();
    Serial.printf("TEST OK balance %.9f %.6f\n",
                  walletSolBalance(), walletUsdcAmount());
    return;
  }
```

- [ ] **Step 2:** Flash.

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 3:** In `tests/run.py`, add the imports and cases. Imports go at the top.

```python
import base58
from solana.rpc.api import Client as SolanaClient
from solders.pubkey import Pubkey
```

If `base58` isn't installed, add `base58>=2.1` to `tests/requirements.txt` and `pip install -r tests/requirements.txt`.

```python
def c_wallet_pubkey_valid(dev: Device) -> str:
    resp = dev.send("TEST WALLET PUBKEY", timeout=3.0)[0].split()
    assert resp[:3] == ["TEST", "OK", "pubkey"], f"bad response: {resp}"
    pk_b58 = resp[3]
    decoded = base58.b58decode(pk_b58)
    assert len(decoded) == 32, f"pubkey decodes to {len(decoded)} bytes, expected 32"
    short = pk_b58[:4] + "…" + pk_b58[-4:]
    return f"({short})"

# Shared across cases that need the device's pubkey + an RPC handle.
def _rpc(args_rpc: str) -> SolanaClient:
    return SolanaClient(args_rpc)

def _device_pubkey(dev: Device) -> str:
    resp = dev.send("TEST WALLET PUBKEY", timeout=3.0)[0].split()
    return resp[3]

def c_wallet_balance_matches(dev: Device, rpc_url: str = DEFAULT_RPC) -> str:
    # Device side.
    resp = dev.send("TEST WALLET BALANCE", timeout=5.0)[0].split()
    assert resp[:3] == ["TEST", "OK", "balance"], f"bad response: {resp}"
    dev_sol = float(resp[3])

    # RPC side, independently.
    pubkey = Pubkey.from_string(_device_pubkey(dev))
    rpc = _rpc(rpc_url)
    lamports = rpc.get_balance(pubkey).value
    rpc_sol = lamports / 1e9

    tol = 0.0001   # 0.0001 SOL ≈ 100k lamports; comfortable rounding slack
    assert abs(dev_sol - rpc_sol) < tol, (
        f"device reports {dev_sol:.6f} SOL, RPC reports {rpc_sol:.6f} SOL")
    return f"({dev_sol:.6f} SOL)"
```

Note: `run_case` only passes `dev`, not `rpc_url`. To thread the RPC URL from `main()`, wrap the case in a lambda in the `CASES` list — see next step.

- [ ] **Step 4:** Update `main()` to thread `args.rpc` into the wallet case. Change the `CASES` list construction to be built inside `main()` after args are parsed:

```python
def main() -> int:
    # ... existing argparse ...

    # Build cases here so x402 / rpc cases can close over args.
    cases: list[tuple[str, Callable[[Device], str]]] = [
        ("liveness",              c_liveness),
        ("boot_heap_ok",          c_boot_heap_ok),
        ("version_reports",       c_version_reports),
        ("screen_roundtrip",      c_screen_roundtrip),
        ("paint_under_budget",    c_paint_under_budget),
        ("menu_tap_wallet",       c_menu_tap_wallet),
        ("menu_tap_info",         c_menu_tap_info),
        ("settings_from_swipe",   c_settings_from_swipe),
        ("wifi_status_connected", c_wifi_status_connected),
        ("wifi_scan_nonempty",    c_wifi_scan_nonempty),
        ("wallet_pubkey_valid",   c_wallet_pubkey_valid),
        ("wallet_balance_matches",lambda d: c_wallet_balance_matches(d, args.rpc)),
    ]

    # ... rest unchanged, iterating `cases` instead of `CASES` ...
```

Delete the module-level `CASES` list (it's now empty anyway).

- [ ] **Step 5:** Run.

```bash
python tests/run.py --only wallet
```

Expected: both PASS. Check that the RPC round-trip takes well under the 5 s `wallet_balance_matches` allows.

- [ ] **Step 6:** Commit.

```bash
git add src/testharness.cpp tests/run.py tests/requirements.txt
git commit -m "test-harness: WALLET verbs + pubkey/balance cases"
```

---

## Task 14: AI PING verb and case

**Files:**
- Modify: `src/testharness.cpp`
- Modify: `tests/run.py`

- [ ] **Step 1:** Identify the AI entry point confirmed in Task 1 Step 4. It's typically a function like `aiAsk(const String &prompt, String &out)` or similar. Adjust the snippet below to match.

In `src/testharness.cpp`, add `#include "ai.h"` at the top, then add the verb:

```cpp
  if (rest == "AI PING") {
    uint32_t t0 = millis();
    String reply;
    // Replace aiAsk / out args to match the actual signature from ai.h.
    bool ok = aiAsk("ping", reply);
    uint32_t dt = millis() - t0;
    if (!ok) {
      Serial.printf("TEST ERR ai no_response dt=%u\n", (unsigned)dt);
      return;
    }
    // Protocol keeps http_status even though we don't bubble it up from
    // the ai module. Use 200 on success as a sentinel.
    Serial.printf("TEST OK ai 200 %u\n", (unsigned)dt);
    return;
  }
```

If `ai.h` doesn't expose a suitable single-call wrapper, spend a few minutes to add one — it's a 10-line thin wrapper around the existing Gemini call site. Keep the test harness the only caller for now.

- [ ] **Step 2:** Flash.

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 3:** Add the case in `tests/run.py`.

```python
def c_ai_ping(dev: Device) -> str:
    resp = dev.send("TEST AI PING", timeout=15.0)[0].split()
    assert resp[:3] == ["TEST", "OK", "ai"], f"bad response: {resp}"
    status = int(resp[3])
    latency_ms = int(resp[4])
    assert status == 200, f"ai status {status}"
    assert latency_ms < 8000, f"ai took {latency_ms} ms (budget 8000)"
    return f"({status}, {latency_ms} ms)"
```

Add to the `cases` list (in `main()`):

```python
    ("ai_ping", c_ai_ping),
```

- [ ] **Step 4:** Run.

```bash
python tests/run.py --only ai_ping
```

Expected: PASS in under 8 s.

- [ ] **Step 5:** Commit.

```bash
git add src/testharness.cpp tests/run.py
git commit -m "test-harness: AI PING verb and case"
```

---

## Task 15: X402 CALL verb and three paid cases

**Files:**
- Modify: `src/testharness.cpp`
- Modify: `tests/run.py`

- [ ] **Step 1:** In `src/testharness.cpp`, add `#include "x402.h"` at the top, then add the verb:

```cpp
  if (rest.startsWith("X402 CALL ")) {
    String url = rest.substring(strlen("X402 CALL "));
    uint32_t t0 = millis();
    // GET is the common shape for the endpoints we care about; if one of
    // the production call sites needs POST, branch on URL substring.
    X402Result r = x402Get(url);
    uint32_t dt = millis() - t0;
    if (r.status == 0) {
      Serial.printf("TEST ERR x402 %s dt=%u\n", r.error.c_str(), (unsigned)dt);
      return;
    }
    // paid_usdc_base = 6-decimal USDC base units (e.g. 50000 = $0.05).
    uint64_t paid_base = (uint64_t)llround(r.costUsd * 1'000'000);
    Serial.printf("TEST OK x402 %d %llu %u\n",
                  r.status, (unsigned long long)paid_base, (unsigned)dt);
    return;
  }
```

- [ ] **Step 2:** Flash.

```bash
pio run -e waveshare_esp32s3_28 -t upload
```

- [ ] **Step 3:** Populate `X402_URLS` at the top of `tests/run.py` with the three URLs you identified in Task 1 Step 3. Example (replace with real):

```python
X402_URLS: list[tuple[str, str]] = [
    ("quote",   "https://daemon-api.example/quote"),
    ("image",   "https://daemon-api.example/image/tiny"),
    ("catalog", "https://daemon-api.example/services/catalog"),
]
```

- [ ] **Step 4:** Add the three case factories and a shared precondition helper.

```python
def _usdc_precondition(dev: Device) -> Optional[str]:
    """Return None if the wallet has ≥ $0.05 USDC, else a skip reason."""
    resp = dev.send("TEST WALLET BALANCE", timeout=5.0)[0].split()
    usdc = float(resp[4])
    if usdc < 0.05:
        return f"needs ≥ $0.05 USDC, have ${usdc:.4f}"
    return None

def _make_x402_case(label: str, url: str):
    def fn(dev: Device) -> str:
        skip = _usdc_precondition(dev)
        if skip:
            raise AssertionError(f"SKIP: {skip}")
        resp = dev.send(f"TEST X402 CALL {url}", timeout=25.0)[0].split()
        assert resp[:3] == ["TEST", "OK", "x402"], f"bad response: {resp}"
        status      = int(resp[3])
        paid_base   = int(resp[4])
        latency_ms  = int(resp[5])
        assert status == 200, f"http {status}"
        # $0.01–$1.00 inclusive — below this means free, above means a
        # mis-priced endpoint that we do NOT want the device paying for.
        assert 10_000 <= paid_base <= 1_000_000, f"paid {paid_base} µUSDC is out of range"
        return f"({status}, ${paid_base/1e6:.4f}, {latency_ms} ms)"
    fn.__name__ = f"c_x402_{label}"
    return fn
```

- [ ] **Step 5:** Add the three cases to the `cases` list in `main()`.

```python
    *[
        (f"x402_payment_{i+1}", _make_x402_case(label, url))
        for i, (label, url) in enumerate(X402_URLS)
    ],
```

- [ ] **Step 6:** Run.

```bash
python tests/run.py --only x402
```

Expected: all three PASS. Each costs a few cents of USDC. If any fails with `http 402` or a signing error, read the device's serial trail (printed on FAIL) — likely either a stale blockhash or a facilitator issue.

- [ ] **Step 7:** Verify `--skip-x402` skips all three.

```bash
python tests/run.py
python tests/run.py --skip-x402
```

Expected: full run includes three `[PASS]` x402 lines; skip run shows three `[SKIP]` lines.

- [ ] **Step 8:** Commit.

```bash
git add src/testharness.cpp tests/run.py
git commit -m "test-harness: X402 CALL verb + 3 paid cases"
```

---

## Task 16: `heap_no_leak` case

**Why last:** this case compares heap-free at the end of a run against heap-free at the start, so it depends on every earlier case existing.

**Files:**
- Modify: `tests/run.py`

- [ ] **Step 1:** Rework `main()` so heap readings bracket the run. Add this around the case loop:

```python
    # Heap before/after, to detect leaks across the whole run.
    heap_before = int(dev.send("TEST HEAP")[0].split()[3])

    # ... existing `for name, fn in cases:` loop ...

    heap_after = int(dev.send("TEST HEAP")[0].split()[3])
    delta = heap_after - heap_before
    if abs(delta) < 20 * 1024:
        print(f"[PASS] heap_no_leak              (delta {delta:+d} B)")
        passed += 1
    else:
        print(f"[FAIL] heap_no_leak              (delta {delta:+d} B; budget ±20 KB)")
        failed += 1
```

This is an inline check rather than a case function because it observes state outside any single case — it'd be wrong to put it in the list and run it mid-sequence.

- [ ] **Step 2:** Run full.

```bash
python tests/run.py --skip-x402
```

Expected: ends with `[PASS] heap_no_leak  (delta +1234 B)` (well under 20 KB).

- [ ] **Step 3:** Commit.

```bash
git add tests/run.py
git commit -m "tests: heap_no_leak delta check around the case loop"
```

---

## Task 17: End-to-end dry run + regression drill

**Why:** the spec's success criteria explicitly asks to confirm the suite catches the original async-scan regression. This task is the proof.

**Files:** none modified in the source tree — this task is testing the test.

- [ ] **Step 1:** Full run on current `main` (no `--skip-x402`).

```bash
python tests/run.py
```

Expected: 16 passed, 0 failed, 0 skipped, exit 0. Total runtime ~45 s. If any case flakes, re-run once; if it flakes twice, debug — we don't paper over flakes.

- [ ] **Step 2:** Regression drill — prove the suite catches the original bug. Temporarily revert `src/wifiscreen.cpp`'s sync scan to async (or just force-return 0 from the scan). Then run:

```bash
python tests/run.py --only wifi_scan_nonempty
```

Expected: `[FAIL] wifi_scan_nonempty  scan returned 0 networks (regression!)`, exit code 1.

- [ ] **Step 3:** Restore `wifiscreen.cpp`, flash, re-run to confirm green.

```bash
git checkout src/wifiscreen.cpp
pio run -e waveshare_esp32s3_28 -t upload
python tests/run.py --only wifi
```

Expected: both wifi cases PASS.

- [ ] **Step 4:** Update `tests/README.md` with the real sample output you just saw (replace placeholder values if you used any in Task 5).

- [ ] **Step 5:** Commit final docs.

```bash
git add tests/README.md
git commit -m "tests: refresh README with verified sample output"
```

---

## Task 18: Optional spec-alignment addendum

**Why:** two deviations were made in the protocol vs. what the spec text says (wallet balance as float; URL-based x402). If the user prefers the spec document match what was implemented, update it here.

**Files:**
- Modify: `docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md`

- [ ] **Step 1:** In the Firmware protocol table, change the `TEST WALLET BALANCE` row's response to:

```
TEST OK balance <sol_float> <usdc_float>
```

And the `TEST X402 CALL` row reflects that the argument is a full URL (already correct).

- [ ] **Step 2:** Commit.

```bash
git add docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md
git commit -m "spec: align wallet-balance row with implemented protocol"
```

This task is **optional**. Skip if the user is fine with the spec reading aspirationally.

---

## Self-review — spec coverage checklist

| Spec requirement                                        | Task(s)             |
| ------------------------------------------------------- | ------------------- |
| `TEST BEGIN / END / PING`                               | Task 3              |
| `TEST HEAP / VERSION`                                   | Task 7              |
| `TEST SCREEN GET / FORCE / PAINT`                       | Tasks 8, 9          |
| `TEST TAP / SWIPE`                                      | Tasks 10, 11        |
| `TEST WIFI STATUS / SCAN`                               | Task 12             |
| `TEST WALLET PUBKEY / BALANCE`                          | Task 13             |
| `TEST AI PING`                                          | Task 14             |
| `TEST X402 CALL <url>`                                  | Task 15             |
| 16 named cases (`boot_heap_ok` → `heap_no_leak`)         | Tasks 6, 7, 9, 11, 12, 13, 14, 15, 16 |
| Single `tests/run.py`, no framework                     | Task 6 onwards      |
| `src/testharness.{h,cpp}`, always compiled, BEGIN-gated | Tasks 2, 3, 4       |
| `tests/requirements.txt`                                | Task 5              |
| `tests/README.md`                                       | Tasks 5, 17         |
| Exit codes 0/1/2                                        | Task 6              |
| `--skip-x402`, `--only`, `--port`, `--rpc`               | Tasks 6, 13, 15     |
| No changes to `platformio.ini`                          | (none)              |
| Regression drill against async-scan                     | Task 17             |

Every spec requirement maps to at least one task. No placeholders remaining. Function / class names introduced in earlier tasks (`Device`, `run_case`, `_usdc_precondition`, `mainInjectTap`, etc.) are used consistently in later tasks.

## Total effort estimate

- Firmware tasks: 11 flashes × ~30 s flash + ~2 min coding each ≈ 40 min.
- Host tasks: written inline, each ~2–3 min ≈ 30 min.
- Regression drill + docs: 15 min.
- **Total: ~90 min** of focused work, assuming the discovery phase (Task 1) doesn't surface surprises.
