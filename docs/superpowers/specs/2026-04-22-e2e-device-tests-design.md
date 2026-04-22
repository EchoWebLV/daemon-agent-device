# Device E2E smoke-test suite

## Goal

Give the Daemon board an automated "did I break anything?" button that runs
from a developer laptop over USB after any major firmware change. The trigger
for this spec was a real regression: an async `WiFi.scanNetworks` call started
returning zero results after the device associated with an AP, and the only
way to notice was to manually open Settings → Wi-Fi and scan. We want to stop
finding regressions like that by hand.

Scope of this spec: one host-side Python driver, one firmware-side test-mode
module, a line-based command protocol over the existing USB-Serial CDC link,
and a fixed list of 16 test cases covering boot, UI navigation, Wi-Fi, wallet,
Gemini AI ping, and three real x402 payments. No hardware rig, no CI
integration, no unit-test framework — just `python tests/run.py` on a dev
machine with the board plugged in.

Explicitly out of scope: a hardware-in-the-loop rig, simulated touch on the
CST328 controller, pixel-diff UI snapshots, a test pyramid with unit tests,
and any kind of production CI. If we want any of those later, they're a
separate spec.

## Current state (what we're starting from)

- Single PlatformIO env `[env:waveshare_esp32s3_28]` in `platformio.ini`.
  No test env, no pytest-embedded, no Unity.
- Firmware already streams human-readable diagnostics over the native USB
  CDC port at 115200 baud (`ARDUINO_USB_CDC_ON_BOOT=1`, `Serial.printf` is
  used everywhere).
- State machine in `src/main.cpp` with six screens
  (`SCREEN_CREATURE / MENU / WALLET / INFO / SETTINGS / WIFI`) and a single
  `switchScreen(target)` dispatcher that calls each screen's `OnEnter`.
- Each screen module exposes `*Begin`, `*OnEnter`, `*Draw`, `*Tick`,
  `*HandleTap(x,y)`, and consumer methods (`*ConsumeClose`,
  `*ConsumeWalletTap`, …). Taps are injected through the touch driver; we
  can reuse the same entry points from a test harness.
- Wi-Fi is driven from `src/server.cpp` (STA mode, sync scan in
  `logNearbyNetworks`) and from `src/wifiscreen.cpp` (now also sync after
  the regression that motivated this work).
- Wallet state lives behind `walletPubkey()`, `walletCanSign()`, and
  `walletBalanceLamports()` (see `src/wallet.h`). Independent on-chain
  truth is reachable from the laptop via `solana-py`.
- x402 paid endpoints: the device currently signs and posts x402 payment
  headers for Gemini + a small service catalog. Production URLs are
  stable; we'll exercise three of them.

## Non-goals

- **No framework.** Plain Python standard lib + `pyserial` + `requests` +
  `solana-py`. No pytest, no pytest-embedded, no Unity, no Espressif
  test runner. One file, top-to-bottom, readable in a single sitting.
- **No mocking.** Tests hit the real device, the real Solana mainnet
  account, and the real x402 production endpoints. A regression that only
  shows up against real infra is the whole point.
- **No firmware variants.** The test harness is always compiled in
  (`#ifdef ENABLE_TEST_HARNESS` would bit-rot the minute we forget to
  flash the test build). It sits idle until the host sends `TEST BEGIN`
  on the CDC line, so it costs nothing at runtime.
- **No retries.** A flaky test is a bug, not a feature to paper over.
  If `wifi scan` returns empty once, we want to see that and investigate.

## Architecture

```
+---------------------------+        USB-CDC         +---------------------------+
|  tests/run.py             |  <------------------>  |  Daemon ESP32-S3          |
|  (plain Python, ~150 LOC) |    115200 baud,        |                           |
|                           |    line-based ASCII    |  src/testharness.cpp      |
|  - enumerates port        |                        |  - `TEST BEGIN` puts main |
|  - `TEST BEGIN`           |                        |    loop in test mode      |
|  - runs 16 cases          |                        |  - dispatches verbs to    |
|  - prints pass/fail       |                        |    screen/wifi/wallet/x402|
|  - exits 0/1/2            |                        |  - `TEST END` resumes UI  |
|                           |                        |                           |
|  Needs: solana-py,        |                        |  Always compiled in;      |
|  requests, pyserial       |                        |  zero cost until BEGIN.   |
+---------------------------+                        +---------------------------+
```

### Split of responsibility

- The **firmware harness** only exposes low-level verbs: *"force screen
  X"*, *"inject a tap at (x,y)"*, *"run a sync Wi-Fi scan and list
  results"*, *"print wallet pubkey"*. It does **no** assertions.
- The **host runner** owns all expectations. It decides what "PASS" means
  (e.g. "at least one SSID returned"), it knows which x402 URL to hit,
  it knows what the wallet balance minimum is. Firmware stays dumb.
- This matters because the moment the firmware starts encoding "the
  right answer", we can't rewrite a test without reflashing.

### Test mode gating

`TEST BEGIN` flips a single static flag in `main.cpp`'s loop: when the
flag is true, `loop()` skips creature animation, audio ticks, and the
swipe-gesture dispatcher, and instead polls `testHarnessTick()` which
reads lines from `Serial`. `TEST END` clears the flag and calls
`switchScreen(SCREEN_CREATURE)` so the device is back to a clean state.

While the flag is set, the firmware is not "broken" — the display just
keeps showing whatever the last `screen force` command painted, and
tap/swipe events from the actual touchscreen are ignored so a stray
finger doesn't corrupt a test run.

## Firmware protocol

Line-oriented, ASCII-only, one verb per line, responses match one-to-one.
Every response line starts with `TEST OK` or `TEST ERR`. No JSON, no
escaping, no framing. Arguments are space-separated. Any text the device
prints outside of a `TEST ...` line is normal `Serial.printf` diagnostic
output and the host ignores it (but logs it when a case fails, so we can
see what the device was saying when it broke).

| Host sends                       | Device responds                                                          | Purpose                                              |
| -------------------------------- | ------------------------------------------------------------------------ | ---------------------------------------------------- |
| `TEST BEGIN`                     | `TEST OK begin`                                                          | Enter test mode. Pauses normal `loop()`.             |
| `TEST END`                       | `TEST OK end`                                                            | Leave test mode. Returns to creature screen.         |
| `TEST PING`                      | `TEST OK ping <uptime_ms>`                                               | Liveness check.                                      |
| `TEST HEAP`                      | `TEST OK heap <free_heap_bytes> <free_psram_bytes>`                      | Memory smoke.                                        |
| `TEST VERSION`                   | `TEST OK version <sdk> <build_date> <build_time>`                        | Sanity on what's actually flashed.                   |
| `TEST SCREEN GET`                | `TEST OK screen <name>`  (creature / menu / wallet / info / settings / wifi) | Which screen the state machine thinks it's on.    |
| `TEST SCREEN FORCE <name>`       | `TEST OK screen <name>`                                                  | Jump to a screen without touching the UI.            |
| `TEST SCREEN PAINT`              | `TEST OK paint <ms>`                                                     | Force a full repaint; returns how long it took.      |
| `TEST TAP <x> <y>`               | `TEST OK tap`                                                            | Synthesize a tap at the given pixel.                 |
| `TEST SWIPE <UP\|DOWN\|LEFT\|RIGHT>` | `TEST OK swipe`                                                      | Synthesize a swipe gesture on the current screen.    |
| `TEST WIFI STATUS`               | `TEST OK wifi <status> <ssid\|-> <rssi\|0>`                              | Connection state without scanning.                   |
| `TEST WIFI SCAN`                 | `TEST OK wifi scan <n>` then `n` lines `TEST NET <ssid> <rssi> <enc>`    | Sync scan, full result set.                          |
| `TEST WALLET PUBKEY`             | `TEST OK pubkey <base58>`                                                | On-device wallet address.                            |
| `TEST WALLET BALANCE`            | `TEST OK balance <lamports>`                                             | Device's own view of SOL balance.                    |
| `TEST AI PING`                   | `TEST OK ai <http_status> <latency_ms>` or `TEST ERR ai <reason>`        | Round-trip a trivial prompt through Gemini.          |
| `TEST X402 CALL <url>`           | `TEST OK x402 <http_status> <paid_usdc_base> <latency_ms>`               | Drive one paid request end-to-end. `paid_usdc_base` is USDC in 6-decimal base units (e.g. 50000 = $0.05). |
| anything else                    | `TEST ERR unknown <verb>`                                                | Never hang, always answer.                           |

Timeouts: the host sets a verb-specific deadline (see the table in
§ "Host runner"). If no response arrives in time, the host flags the
case `FAIL: timeout`, calls `TEST END`, closes the port, and moves on.

## Host runner structure

Single file `tests/run.py`, around 150 lines, no classes beyond a tiny
`Device` wrapper. Pseudocode:

```python
# tests/run.py (shape only)
import argparse, sys, time, serial, serial.tools.list_ports
import requests
from solders.pubkey import Pubkey
from solana.rpc.api import Client

BAUD = 115200
DEFAULT_RPC = "https://api.mainnet-beta.solana.com"
# Real production URLs — these are the same endpoints the firmware hits
# in normal use. Pulled from src/server.cpp / src/aiclient.cpp at
# implementation time; keeping them in one place here so the runner
# and the firmware never drift.
X402_URLS = [
    # ("quote",   "https://..."),
    # ("image",   "https://..."),
    # ("catalog", "https://..."),
]

class Device:
    def __init__(self, port): ...
    def send(self, line, timeout=2.0) -> list[str]: ...
    def begin(self): ...
    def end(self):   ...

def find_port() -> str: ...          # picks the CDC port, dies if >1 match
def run_case(dev, name, fn) -> bool: ...

# --- individual cases ----------------------------------------------------

def c_boot_heap_ok(dev):        ...  # TEST HEAP, assert free_heap > 80 KB
def c_version_reports(dev):     ...  # TEST VERSION, assert non-empty
def c_screen_roundtrip(dev):    ...  # FORCE each screen, GET confirms
def c_menu_tap_wallet(dev):     ...  # FORCE menu; TAP wallet tile; GET == wallet
def c_menu_tap_info(dev):       ...
def c_settings_from_swipe(dev): ...  # FORCE creature; SWIPE DOWN; GET == settings
def c_wifi_scan_nonempty(dev):  ...  # SCAN, assert at least one SSID
def c_wifi_status_connected(dev): ...
def c_wallet_pubkey_valid(dev): ...  # base58, 32 bytes when decoded
def c_wallet_balance_matches(dev): ... # RPC getBalance vs TEST WALLET BALANCE
def c_ai_ping(dev):             ...  # TEST AI PING, assert http 200, < 8 s
def c_paint_under_budget(dev):  ... # TEST SCREEN PAINT on each screen, < 60 ms
def c_x402_payment_1(dev):      ...  # TEST X402 CALL <url>, assert 200
def c_x402_payment_2(dev):      ...
def c_x402_payment_3(dev):      ...
def c_heap_no_leak(dev):        ...  # HEAP before/after all of the above

CASES = [
    ("boot_heap_ok",          c_boot_heap_ok),
    ("version_reports",       c_version_reports),
    ("screen_roundtrip",      c_screen_roundtrip),
    ("menu_tap_wallet",       c_menu_tap_wallet),
    ("menu_tap_info",         c_menu_tap_info),
    ("settings_from_swipe",   c_settings_from_swipe),
    ("wifi_scan_nonempty",    c_wifi_scan_nonempty),
    ("wifi_status_connected", c_wifi_status_connected),
    ("wallet_pubkey_valid",   c_wallet_pubkey_valid),
    ("wallet_balance_matches",c_wallet_balance_matches),
    ("ai_ping",               c_ai_ping),
    ("paint_under_budget",    c_paint_under_budget),
    ("x402_payment_1",        c_x402_payment_1),
    ("x402_payment_2",        c_x402_payment_2),
    ("x402_payment_3",        c_x402_payment_3),
    ("heap_no_leak",          c_heap_no_leak),
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port",      default=None, help="override auto-detect")
    ap.add_argument("--skip-x402", action="store_true", help="skip 3 paid cases")
    ap.add_argument("--only",      default=None, help="substring filter on case name")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("no device found"); sys.exit(2)

    dev = Device(port); dev.begin()
    try:
        passed = failed = 0
        for name, fn in CASES:
            if args.skip_x402 and name.startswith("x402_"): continue
            if args.only and args.only not in name:         continue
            ok = run_case(dev, name, fn)
            passed += int(ok); failed += int(not ok)
        print(f"\n{passed} passed, {failed} failed")
        sys.exit(0 if failed == 0 else 1)
    finally:
        dev.end()
```

### Per-verb timeouts

| Verb                 | Timeout | Rationale                                                |
| -------------------- | ------- | -------------------------------------------------------- |
| BEGIN / END / PING   |   2 s   | Should be instant.                                        |
| HEAP / VERSION       |   2 s   |                                                          |
| SCREEN GET / FORCE   |   3 s   | Forcing a screen runs `OnEnter` incl. slide-in ~220 ms.  |
| SCREEN PAINT         |   3 s   |                                                          |
| TAP / SWIPE          |   2 s   |                                                          |
| WIFI STATUS          |   2 s   |                                                          |
| WIFI SCAN            |  10 s   | ESP32 sync scan typically 2–4 s.                         |
| WALLET PUBKEY / BAL  |   3 s   |                                                          |
| AI PING              |  10 s   | Gemini round-trip.                                       |
| X402 CALL            |  20 s   | Payment build + sign + submit + server fetch + verify.   |

### Exit codes

| Code | Meaning                                             |
| ---- | --------------------------------------------------- |
|  0   | Every selected case passed.                         |
|  1   | At least one case failed (normal test-suite fail).  |
|  2   | Device not found, multiple ports match, or serial port couldn't be opened. |

## Test cases

Sixteen cases, grouped. Each case starts by checking the preconditions it
needs (e.g. `x402_*` skip with an informative message if RPC shows wallet
balance below the $0.05 USDC threshold) so a missing precondition reads
as `SKIP`, not `FAIL`.

| # | Name                        | What it proves                                                                 |
| - | --------------------------- | ------------------------------------------------------------------------------ |
| 1 | `boot_heap_ok`              | Device responds and has enough free heap + PSRAM for the audio buffer.         |
| 2 | `version_reports`           | Build metadata is present and sane (`__DATE__` / `__TIME__` not blank).        |
| 3 | `screen_roundtrip`          | `SCREEN FORCE` → `SCREEN GET` matches for all six screens.                     |
| 4 | `menu_tap_wallet`           | From menu, tapping the Wallet tile's center transitions to wallet.             |
| 5 | `menu_tap_info`             | From menu, tapping the Info tile's center transitions to info.                 |
| 6 | `settings_from_swipe`       | From creature, `SWIPE DOWN` opens settings.                                    |
| 7 | `wifi_scan_nonempty`        | Sync scan returns ≥ 1 network (guards against the async-scan regression).      |
| 8 | `wifi_status_connected`     | Device reports `WL_CONNECTED` with a non-empty SSID and non-zero RSSI.         |
| 9 | `wallet_pubkey_valid`       | Pubkey is 32 bytes when base58-decoded.                                        |
| 10| `wallet_balance_matches`    | On-device balance equals `Client(RPC).get_balance(pubkey)` exactly (queries are milliseconds apart; mismatch means the device is reading a different account than its public key says).    |
| 11| `ai_ping`                   | A trivial Gemini prompt returns HTTP 200 within 8 s.                           |
| 12| `paint_under_budget`        | Every screen repaints in < 60 ms (we paint once on entry; this is slack).      |
| 13| `x402_payment_1`            | Quote endpoint returns 200 and the facilitator accepted the signed payment.    |
| 14| `x402_payment_2`            | Image endpoint, same contract.                                                 |
| 15| `x402_payment_3`            | Catalog endpoint, same contract.                                               |
| 16| `heap_no_leak`              | `free_heap` at the end is within 20 KB of `free_heap` after case 1.            |

A case is a pure function that takes the `Device` and raises on failure.
`run_case` catches the exception, prints `FAIL: <case>: <reason>` with any
buffered Serial chatter from the device, and returns `False`.

### x402 notes

- The three URLs are real production endpoints that this device already
  uses in normal operation. We deliberately don't mock them — the whole
  point is catching regressions that only show up against the live
  facilitator / RPC / signer stack.
- Each case requires `≥ $0.05` USDC on the wallet. The runner checks this
  once up front (via `solana-py` + the USDC SPL balance RPC) and skips
  all x402 cases with a single clear message if short.
- `--skip-x402` is there for iteration speed while hacking on UI code;
  expected minimum cost per full run is ~3 × facilitator fee (cents).

## How to run

### Preconditions

1. Device is flashed with current `main` and connected to the laptop via USB.
2. Device has previously joined a Wi-Fi network (stored in NVS).
3. Device's wallet holds ≥ 0.001 SOL and ≥ $0.05 USDC on mainnet (the
   runner checks and will skip x402 cases otherwise).
4. Laptop can reach the public internet (for the RPC and x402 endpoints).
5. Python 3.11+ with `pip install -r tests/requirements.txt`:
   ```
   pyserial>=3.5
   requests>=2.31
   solana>=0.34
   solders>=0.20
   ```

### Commands

```bash
# full run — 16 cases, ~45 s on a good link
python tests/run.py

# fast iteration while hacking on UI code — 13 cases, ~10 s
python tests/run.py --skip-x402

# pick a subset
python tests/run.py --only wifi
python tests/run.py --only x402
python tests/run.py --only screen_roundtrip

# override port auto-detect (e.g. two boards plugged in)
python tests/run.py --port /dev/cu.usbmodem2101
```

### Sample output

```
$ python tests/run.py --skip-x402
device: /dev/cu.usbmodem101 (ESP32-S3)
-> TEST BEGIN

[PASS] boot_heap_ok              (free_heap=198 KB, psram=6.8 MB)
[PASS] version_reports           (2026-04-22 13:11:02)
[PASS] screen_roundtrip          (6/6)
[PASS] menu_tap_wallet
[PASS] menu_tap_info
[PASS] settings_from_swipe
[PASS] wifi_scan_nonempty        (7 networks)
[PASS] wifi_status_connected     (MyAP, -54 dBm)
[PASS] wallet_pubkey_valid       (HN7cABP...LqgB)
[PASS] wallet_balance_matches    (12_340_000 lamports)
[PASS] ai_ping                   (200, 1820 ms)
[PASS] paint_under_budget        (max 31 ms)
[SKIP] x402_payment_1            (--skip-x402)
[SKIP] x402_payment_2            (--skip-x402)
[SKIP] x402_payment_3            (--skip-x402)
[PASS] heap_no_leak              (delta=+1 KB)

13 passed, 0 failed, 3 skipped
<- TEST END
```

## File layout

```
src/
  testharness.h       # public API: testHarnessBegin(), testHarnessTick()
  testharness.cpp     # ~250 LOC, implements every TEST verb above
  main.cpp            # +~15 LOC: call testHarnessBegin(), delegate in loop()
tests/
  run.py              # the whole host runner (~150 LOC)
  requirements.txt    # pyserial, requests, solana, solders
  README.md           # one-pager: preconditions, `python tests/run.py`
```

No changes to `platformio.ini`. No second build profile. No
`lib/` restructuring. The test harness sits next to the screens.

## Risks and mitigations

- **Firmware harness always compiled** adds a few KB of flash and one
  `Serial.readStringUntil('\n')` per `loop()` iteration. Mitigation:
  short-circuit the check with a single `if (!s_testMode) return;` at
  the top of `testHarnessTick`, guard the line-read behind
  `Serial.available()`.
- **USB CDC line noise** could split a response across reads. Mitigation:
  host reads lines (`readline()`), not bytes; firmware writes full lines
  with `Serial.println`; `readline()` with a deadline gives us back one
  logical line or a timeout.
- **Multiple boards plugged in** makes `find_port()` ambiguous.
  Mitigation: if `list_ports.comports()` returns more than one candidate
  matching the CP210x/CDC vendor-id pattern, exit 2 with a message
  telling the user to pass `--port`.
- **Drift between what the harness can do and what production code
  does** — e.g. the harness gets a `TEST WALLET BALANCE` verb but calls
  a private API. Mitigation: every harness verb calls the same public
  function that the UI calls (`walletBalanceLamports`,
  `WiFi.scanNetworks(false, true)`, etc.). If the UI would see a stale
  value, the test sees the stale value too. That's the point.
- **x402 flakes** against live infra will produce false failures.
  Mitigation: `--skip-x402` is the fast path; when debugging a paid
  case, the FAIL output includes the raw HTTP status and the device's
  `Serial.printf` trail, so we can see whether it was a signing bug, a
  facilitator 402, or a server 500.

## Success criteria

- `python tests/run.py --skip-x402` runs in under 15 s and reports
  pass/fail for 13 cases on the current `main`.
- Re-introducing the async-scan bug (revert `wifiscreen.cpp` to the
  async path) makes `wifi_scan_nonempty` fail and everything else pass.
- Breaking the menu → wallet tap dispatch makes `menu_tap_wallet` fail
  and leaves `screen_roundtrip` green, so the failure pinpoints the
  regression to the menu tap path rather than the screen state machine.
- Running with `--skip-x402` costs zero in SOL/USDC.
