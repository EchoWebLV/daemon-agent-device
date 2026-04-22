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
from typing import Callable, List, Optional, Tuple

import serial
import serial.tools.list_ports

# --- Config ----------------------------------------------------------------

BAUD = 115200
DEFAULT_RPC = "https://api.mainnet-beta.solana.com"

# Filled in Task 15 from `grep x402Post src/*.cpp`. Keep empty until then.
X402_URLS: List[Tuple[str, str]] = [
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
        self.trail: List[str] = []       # non-TEST Serial chatter, for failure logs

    def send(self, line: str, timeout: float = 2.0) -> List[str]:
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
    try:
        detail = fn(dev) or ""
    except AssertionError as e:
        msg = str(e)
        if msg.startswith("SKIP:"):
            print(f"[SKIP] {name:<26}  {msg[5:].strip()}")
            return True  # skip counts as non-failure
        print(f"[FAIL] {name:<26}  {msg}")
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
    print(f"[PASS] {name:<26}  {detail}")
    return True


# --- Cases -----------------------------------------------------------------

def c_liveness(dev: Device) -> str:
    """Round-trip a single PING and extract the uptime report."""
    resp = dev.send("TEST PING", timeout=2.0)
    assert resp and resp[0].startswith("TEST OK ping "), f"bad response: {resp}"
    uptime_ms = int(resp[0].split()[-1])
    return f"(uptime {uptime_ms} ms)"


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
    assert sdk,    "empty sdk"
    assert date,   "empty build date"
    assert time_s, "empty build time"
    return f"({sdk}, built {date} {time_s})"


SCREENS = ["creature", "menu", "wallet", "info", "settings", "wifi"]


def c_screen_roundtrip(dev: Device) -> str:
    """For every known screen: FORCE, then GET, confirm the state changed."""
    for name in SCREENS:
        resp = dev.send(f"TEST SCREEN FORCE {name}", timeout=3.0)
        assert resp and resp[0].endswith(name), f"force {name}: {resp}"
        # Confirm via a separate GET so we're not trusting the FORCE reply.
        resp = dev.send("TEST SCREEN GET", timeout=2.0)
        got = resp[0].split()[-1]
        assert got == name, f"GET after FORCE {name} returned {got}"
    return f"({len(SCREENS)}/{len(SCREENS)} screens)"


def c_paint_under_budget(dev: Device) -> str:
    """Each screen's full repaint must finish under 60 ms. The wifi screen
    gets a longer settle because its first tick kicks off a blocking scan;
    by the time PAINT lands the scan has completed and MODE_LIST is up."""
    worst = 0
    for name in SCREENS:
        dev.send(f"TEST SCREEN FORCE {name}", timeout=3.0)
        # Wi-Fi's first tick blocks in scanNetworks(); give it room.
        settle = 5.0 if name == "wifi" else 0.25
        time.sleep(settle)
        resp = dev.send("TEST SCREEN PAINT", timeout=6.0)
        ms = int(resp[0].split()[-1])
        assert ms < 60, f"{name} repaint took {ms} ms (budget 60)"
        if ms > worst:
            worst = ms
    return f"(max {worst} ms)"


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

    # Build the case list inside main() so cases can close over `args`
    # (e.g. the wallet case needs --rpc, x402 cases need --skip-x402).
    cases: List[Tuple[str, Callable[[Device], str]]] = [
        ("liveness",            c_liveness),
        ("boot_heap_ok",        c_boot_heap_ok),
        ("version_reports",     c_version_reports),
        ("screen_roundtrip",    c_screen_roundtrip),
        ("paint_under_budget",  c_paint_under_budget),
        # Later tasks insert cases here in protocol order.
    ]

    dev.begin()
    print("-> TEST BEGIN\n")
    try:
        passed = failed = skipped = 0
        for name, fn in cases:
            if args.only and args.only not in name:
                continue
            if args.skip_x402 and name.startswith("x402_"):
                print(f"[SKIP] {name:<26}  (--skip-x402)")
                skipped += 1
                continue
            ok = run_case(dev, name, fn)
            if ok:
                passed += 1
            else:
                failed += 1
        print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
        return 0 if failed == 0 else 1
    finally:
        dev.end()
        print("<- TEST END")


if __name__ == "__main__":
    sys.exit(main())
