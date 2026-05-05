#!/usr/bin/env python3
"""
tests/run.py — host driver for the on-device smoke-test harness.

See docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md for the
protocol and test-case contract. Everything the runner needs lives in this
single file on purpose: readable top-to-bottom, no test framework.

Manual E2E checklist (hardware required — run after flashing the pay-sh-dispatcher firmware):

1. Free-tier path — enable AgentMail in web admin -> voice "create a test inbox" ->
   tool fires -> response spoken; no modal.

2. Hold-confirm path — drop auto cap to $0.0001 -> enable a $0.01 provider -> voice a
   request -> modal opens -> hold 3s confirms; release early denies.

3. TTS cue threshold — voice a request that resolves to >= $1 cost -> TTS speaks the
   price BEFORE the modal opens; sub-$1 is silent.

4. Daily-cap refusal — set daily cap = today + small headroom -> voice a request that
   would exceed -> "That'd put you over your spending limits." spoken; counter unchanged.

5. PTT mid-modal — trigger pay-confirm modal -> press PTT -> modal closes DENIED.

6. Reboot persistence — paid call increments counter -> reboot -> settings detail shows
   same today; UTC-midnight rollover resets to 0.
"""

import argparse
import sys
import time
from typing import Callable, List, Optional, Tuple

import base58
import serial
import serial.tools.list_ports
from solana.rpc.api import Client as SolanaClient
from solders.pubkey import Pubkey

# --- Config ----------------------------------------------------------------

BAUD = 115200
DEFAULT_RPC = "https://api.mainnet-beta.solana.com"

# Real paid endpoints. Each tuple is (label, url, min_uusdc, max_uusdc)
# where the µUSDC range is what the endpoint is expected to charge per
# call — tight enough to catch "payment not accounted" regressions
# (floor) without false-positiving on an endpoint whose price rises a
# bit (ceiling at $1).
#
# The three chat hits exist so each call signs a fresh blockhash — direct
# coverage for the bug documented in memory/MEMORY.md (cached blockhashes
# → duplicate-signature tx). Shannon gets one hit at its documented
# $0.00003 price point to cover a second x402 implementation.
_CHAT_URL    = "https://sol.blockrun.ai/api/v1/chat/completions"
_SHANNON_URL = "https://daemon-x402s-seven.vercel.app/api/call"
X402_URLS: List[Tuple[str, str, int, int]] = [
    ("chat1",   _CHAT_URL,       1_000, 1_000_000),   # ~$0.0027 observed
    ("chat2",   _CHAT_URL,       1_000, 1_000_000),
    ("chat3",   _CHAT_URL,       1_000, 1_000_000),
    ("shannon", _SHANNON_URL,       10, 1_000_000),   # ~$0.00003 documented
]


# --- Device wrapper --------------------------------------------------------

class Device:
    """Thin wrapper around pyserial that sends one TEST command and collects
    lines until an OK/ERR response. Multi-line responses (WIFI SCAN) are
    handled by peeking at the `<n>` count and reading exactly N tails."""

    def __init__(self, port: str):
        # Open without asserting DTR/RTS — on the ESP32-S3's native USB-CDC
        # (and on boards that bridge those pins to EN/BOOT), pyserial's
        # default open pulses the chip into reset. Clearing them first
        # keeps the running firmware alive.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = BAUD
        self.ser.timeout = 0.1
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.trail: List[str] = []       # non-TEST Serial chatter, for failure logs
        # Wait for the first TEST reply. Cold boot with WiFi + wallet RPC
        # can take ~20 s before loop() starts, so we budget 30 s.
        #
        # We use BEGIN (not PING) as the readiness probe on purpose: BEGIN
        # flips the device into test mode, which silences the audio task's
        # Serial.print callbacks. Otherwise the audio library keeps
        # chattering from core 0 during boot and scrambles the single-line
        # reply ("[audio] Ch[audio] SaTEST OK ..."). BEGIN is idempotent,
        # so calling it here and not calling it again from main() is fine.
        self.send("TEST BEGIN", timeout=30.0)

    def send(self, line: str, timeout: float = 2.0) -> List[str]:
        # Drop any unread chatter before writing: stray PING echoes from
        # cold-boot readiness polling, audio-task log lines that were
        # mid-flight, etc. Without this a slow earlier reply gets picked
        # up as the answer to the next command.
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            s = raw.decode(errors="replace").rstrip("\r\n")
            # The audio library prints from a different FreeRTOS task, so
            # its output sometimes splices into the middle of our TEST
            # response — e.g. "[audio] BitsPerSamTEST OK ping 15907".
            # Find the protocol marker anywhere in the line and slice
            # from there.
            idx = s.find("TEST OK")
            if idx < 0:
                idx = s.find("TEST ERR")
            if idx >= 0:
                s = s[idx:]
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
                        # Same interleave defense for TEST NET rows.
                        ni = cont.find("TEST NET")
                        if ni >= 0:
                            cont = cont[ni:]
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


# The ESP-IDF build has four screens (creature / wallet / settings / wifi).
# The original Arduino spec listed six (adding menu / info) but those were
# never rebuilt after the ESP-IDF migration — see ui.h comments. We test
# what exists.
SCREENS = ["creature", "wallet", "settings", "wifi"]


def c_screen_roundtrip(dev: Device) -> str:
    """For every known screen: FORCE, then GET, confirm the state changed.
    Ends on `creature` so we don't leave the device mid-scan on the wifi
    screen — that broke paint_under_budget with cross-case state."""
    for name in SCREENS:
        resp = dev.send(f"TEST SCREEN FORCE {name}", timeout=3.0)
        assert resp and resp[0].endswith(name), f"force {name}: {resp}"
        # Confirm via a separate GET so we're not trusting the FORCE reply.
        resp = dev.send("TEST SCREEN GET", timeout=2.0)
        got = resp[0].split()[-1]
        assert got == name, f"GET after FORCE {name} returned {got}"
    # Park on creature as a clean post-state.
    dev.send("TEST SCREEN FORCE creature", timeout=3.0)
    return f"({len(SCREENS)}/{len(SCREENS)} screens)"


def c_paint_under_budget(dev: Device) -> str:
    """Each screen's full repaint must finish under 120 ms. The wifi screen
    gets a longer settle because its first tick kicks off a blocking scan;
    by the time PAINT lands the scan has completed and MODE_LIST is up.
    120 ms is "draw path isn't bloated" — normal hardware paints are 15–60
    ms, and we want the occasional audio/Wi-Fi jitter spike to not trip
    the regression alarm on an otherwise-working build."""
    worst = 0
    last = None
    try:
        for name in SCREENS:
            last = name
            dev.send(f"TEST SCREEN FORCE {name}", timeout=3.0)
            # Wi-Fi's first tick blocks in scanNetworks(); give it room.
            settle = 5.0 if name == "wifi" else 0.25
            time.sleep(settle)
            resp = dev.send("TEST SCREEN PAINT", timeout=6.0)
            ms = int(resp[0].split()[-1])
            assert ms < 120, f"{name} repaint took {ms} ms (budget 120)"
            if ms > worst:
                worst = ms
    except TimeoutError as e:
        # Re-raise with which screen we were on — makes reboot-during-test
        # obvious instead of a generic timeout.
        raise AssertionError(f"while testing screen={last!r}: {e}") from e
    return f"(max {worst} ms)"


def c_settings_from_swipe(dev: Device) -> str:
    """From the creature, swipe RIGHT should land on settings."""
    dev.send("TEST SCREEN FORCE creature", timeout=3.0)
    time.sleep(0.3)
    dev.send("TEST SWIPE RIGHT", timeout=2.0)
    time.sleep(0.3)
    got = dev.send("TEST SCREEN GET", timeout=2.0)[0].split()[-1]
    assert got == "settings", f"expected settings, got {got}"
    return ""


def c_wallet_from_swipe(dev: Device) -> str:
    """From the creature, swipe LEFT should land on the wallet screen. This
    covers the other half of the gesture dispatch that c_settings_from_swipe
    skips, so a regression in LV_DIR_LEFT handling shows up here."""
    dev.send("TEST SCREEN FORCE creature", timeout=3.0)
    time.sleep(0.3)
    dev.send("TEST SWIPE LEFT", timeout=2.0)
    time.sleep(0.3)
    got = dev.send("TEST SCREEN GET", timeout=2.0)[0].split()[-1]
    assert got == "wallet", f"expected wallet, got {got}"
    return ""


def c_wifi_status_connected(dev: Device) -> str:
    """Device must be associated with an AP (credentials from NVS)."""
    resp = dev.send("TEST WIFI STATUS", timeout=3.0)[0].split()
    # Format: TEST OK wifi <connected|disconnected> <ssid_or_-> <rssi>
    assert resp[3] == "connected", f"wifi is {resp[3]}"
    ssid = resp[4].replace("_", " ")
    rssi = int(resp[5])
    assert ssid, "empty ssid"
    assert -100 < rssi < 0, f"implausible rssi {rssi}"
    return f"({ssid}, {rssi} dBm)"


def c_wifi_scan_nonempty(dev: Device) -> str:
    """Regression guard for the async-scan bug: sync scan must find ≥1 AP."""
    # Sync scan takes 2–4 s on ESP32. Give it 10.
    resp = dev.send("TEST WIFI SCAN", timeout=10.0)
    head = resp[0].split()
    n = int(head[-1])
    assert n >= 1, "scan returned 0 networks (regression!)"
    ssids = [ln.split()[2].replace("_", " ") for ln in resp[1:]]
    preview = ", ".join(ssids[:3])
    if n > 3:
        preview += "…"
    return f"({n} networks: {preview})"


def _device_pubkey(dev: Device) -> str:
    """Helper: TEST WALLET PUBKEY → base58 string."""
    resp = dev.send("TEST WALLET PUBKEY", timeout=3.0)[0].split()
    assert resp[:3] == ["TEST", "OK", "pubkey"], f"bad response: {resp}"
    return resp[3]


def c_wallet_pubkey_valid(dev: Device) -> str:
    """Device returns a base58-encoded Ed25519 pubkey (32 bytes decoded)."""
    pk_b58 = _device_pubkey(dev)
    decoded = base58.b58decode(pk_b58)
    assert len(decoded) == 32, f"pubkey decodes to {len(decoded)} bytes, expected 32"
    short = pk_b58[:4] + "…" + pk_b58[-4:]
    return f"({short})"


def c_wallet_balance_matches(dev: Device, rpc_url: str = DEFAULT_RPC) -> str:
    """Device-reported SOL balance must match an independent RPC query
    within 0.0001 SOL (rounding / blockhash timing slack)."""
    # Device side.
    resp = dev.send("TEST WALLET BALANCE", timeout=5.0)[0].split()
    assert resp[:3] == ["TEST", "OK", "balance"], f"bad response: {resp}"
    dev_sol = float(resp[3])

    # RPC side, independently.
    pubkey = Pubkey.from_string(_device_pubkey(dev))
    rpc = SolanaClient(rpc_url)
    lamports = rpc.get_balance(pubkey).value
    rpc_sol = lamports / 1e9

    tol = 0.0001   # ~100k lamports; comfortable rounding slack
    assert abs(dev_sol - rpc_sol) < tol, (
        f"device reports {dev_sol:.6f} SOL, RPC reports {rpc_sol:.6f} SOL")
    return f"({dev_sol:.6f} SOL)"


def c_ai_ping(dev: Device) -> str:
    """Gemini round-trip: one-shot 'ping' prompt, expect success under 8 s."""
    resp = dev.send("TEST AI PING", timeout=15.0)[0].split()
    assert resp[:3] == ["TEST", "OK", "ai"], f"bad response: {resp}"
    status = int(resp[3])
    latency_ms = int(resp[4])
    assert status == 200, f"ai status {status}"
    assert latency_ms < 8000, f"ai took {latency_ms} ms (budget 8000)"
    return f"({status}, {latency_ms} ms)"


def _usdc_precondition(dev: Device) -> Optional[str]:
    """Return None if the wallet has ≥ $0.05 USDC, else a skip reason.
    Relies on TEST WALLET BALANCE returning '<sol> <usdc>' as the 4th/5th
    tokens (see src/testharness.cpp WALLET BALANCE branch)."""
    resp = dev.send("TEST WALLET BALANCE", timeout=5.0)[0].split()
    usdc = float(resp[4])
    if usdc < 0.05:
        return f"needs >= $0.05 USDC, have ${usdc:.4f}"
    return None


def _make_x402_case(
    label: str, url: str, min_uusdc: int, max_uusdc: int,
) -> Callable[[Device], str]:
    """Build a case function that sends TEST X402 CALL <url>, with a
    pre-flight USDC balance check that SKIPs (not fails) the case if the
    wallet is short. `min_uusdc` / `max_uusdc` bracket the endpoint's
    expected price in µUSDC (6-decimal base units) — tight enough to
    catch "payment not accounted" regressions without tripping on small
    price changes."""
    def fn(dev: Device) -> str:
        skip = _usdc_precondition(dev)
        if skip:
            raise AssertionError(f"SKIP: {skip}")
        # 45 s: the x402 library retries internally (402 -> fresh blockhash
        # -> resign -> retry), and the facilitator sometimes pushes us
        # through two retry rounds before a clean 200. Seen in practice:
        # a single paid call can burn 15–20 s across retries.
        resp = dev.send(f"TEST X402 CALL {url}", timeout=45.0)[0].split()
        assert resp[:3] == ["TEST", "OK", "x402"], f"bad response: {resp}"
        status     = int(resp[3])
        paid_base  = int(resp[4])
        latency_ms = int(resp[5])
        assert status == 200, f"http {status}"
        assert min_uusdc <= paid_base <= max_uusdc, (
            f"paid {paid_base} uUSDC out of range "
            f"[{min_uusdc}, {max_uusdc}]")
        # 5 decimals so sub-cent prices (e.g. Shannon at $0.00003) stay
        # visible instead of rounding to $0.0000.
        return f"({status}, ${paid_base / 1e6:.5f}, {latency_ms} ms)"
    fn.__name__ = f"c_x402_{label}"
    return fn


# --- payapi cases ----------------------------------------------------------

def c_payapi_catalog_refresh(dev: Device) -> str:
    """PAYAPI REFRESH_CATALOG: catalog fetch succeeds and reports >= 1 provider."""
    resp = dev.send("TEST PAYAPI REFRESH_CATALOG", timeout=15.0)[0]
    assert "ok=1" in resp, f"got: {resp}"
    return f"({resp.strip()})"


def c_payapi_provider_refresh(dev: Device) -> str:
    """PAYAPI REFRESH_PROVIDER: per-provider openapi fetch includes expected fqn."""
    # Ensure catalog is present first (idempotent).
    dev.send("TEST PAYAPI REFRESH_CATALOG", timeout=15.0)
    resp = dev.send("TEST PAYAPI REFRESH_PROVIDER agentmail/email", timeout=20.0)[0]
    assert "fqn=agentmail/email" in resp, f"got: {resp}"
    return f"({resp.strip()})"


def c_payapi_guard_auto(dev: Device) -> str:
    """PAYAPI GUARD: 50000 micros (~$0.05) is under the default $0.10 auto cap -> AUTO."""
    resp = dev.send("TEST PAYAPI GUARD 50000", timeout=5.0)[0]
    assert "decision=AUTO" in resp, f"got: {resp}"
    return "(decision=AUTO)"


def c_payapi_guard_refuse_over_confirm(dev: Device) -> str:
    """PAYAPI GUARD: 6000000 micros ($6.00) exceeds default $5 confirm cap -> REFUSE."""
    resp = dev.send("TEST PAYAPI GUARD 6000000", timeout=5.0)[0]
    assert "decision=REFUSE" in resp, f"got: {resp}"
    return "(decision=REFUSE)"


def c_spend_today(dev: Device) -> str:
    """SPEND TODAY: returns a non-negative micros counter."""
    resp = dev.send("TEST SPEND TODAY", timeout=3.0)[0]
    assert "micros=" in resp, f"got: {resp}"
    micros = int(resp.split("micros=")[1].split()[0])
    assert micros >= 0, f"negative micros: {micros}"
    return f"(micros={micros})"


def c_spend_add(dev: Device) -> str:
    """SPEND ADD: round-trips a delta; counter increases by exactly that amount."""
    before = int(dev.send("TEST SPEND TODAY", timeout=3.0)[0].split("micros=")[1].split()[0])
    add_resp = dev.send("TEST SPEND ADD 1000", timeout=3.0)[0]
    assert "added" in add_resp, f"ADD replied: {add_resp}"
    after = int(dev.send("TEST SPEND TODAY", timeout=3.0)[0].split("micros=")[1].split()[0])
    assert after == before + 1000, f"expected {before + 1000}, got {after}"
    # Undo: device has no SPEND SUB, but we can note the +1000 is negligible
    # against real budgets and the next test run will just accumulate. Acceptable.
    return f"(before={before}, after={after})"


def _make_spending_http_case(ip: str) -> Callable[[Device], str]:
    """Build a spending GET/POST round-trip case that talks to the device HTTP server.
    Pulled out as a factory so the case can close over the `ip` from CLI args."""
    def fn(dev: Device) -> str:
        import json as J
        import urllib.request

        base = f"http://{ip}/api/spending"

        # Read current state.
        j1 = J.loads(urllib.request.urlopen(base, timeout=5).read())
        assert "auto_max_cents" in j1, f"missing auto_max_cents in {j1}"

        # Write new values.
        body = J.dumps({
            "auto_max_cents":    25,
            "confirm_max_cents": 750,
            "daily_cap_cents":   1500,
        }).encode()
        req = urllib.request.Request(
            base, data=body, headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=5).read()

        # Verify the change.
        j2 = J.loads(urllib.request.urlopen(base, timeout=5).read())
        assert j2["auto_max_cents"] == 25, f"auto_max_cents not updated: {j2}"

        # Restore defaults so subsequent runs aren't surprised.
        restore = J.dumps({
            "auto_max_cents":    10,
            "confirm_max_cents": 500,
            "daily_cap_cents":   1000,
        }).encode()
        req2 = urllib.request.Request(
            base, data=restore, headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req2, timeout=5).read()

        return f"(auto_max_cents 25 confirmed; defaults restored)"
    fn.__name__ = "c_spending_endpoints_round_trip"
    return fn


# --- Main ------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description="Daemon device E2E smoke tests")
    ap.add_argument("--port",      default=None, help="override USB port auto-detect")
    ap.add_argument("--rpc",       default=DEFAULT_RPC, help="Solana RPC URL")
    ap.add_argument("--ip",        default=None, help="device IP for HTTP endpoint tests")
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
        ("liveness",               c_liveness),
        ("boot_heap_ok",           c_boot_heap_ok),
        ("version_reports",        c_version_reports),
        ("screen_roundtrip",       c_screen_roundtrip),
        ("paint_under_budget",     c_paint_under_budget),
        # Gesture dispatch: creature → DOWN → settings and creature → LEFT →
        # wallet. These replace the menu_tap_* cases from the original Arduino
        # spec — the ESP-IDF build doesn't have a menu screen; swipes are
        # the only way to move between creature / wallet / settings.
        ("settings_from_swipe",    c_settings_from_swipe),
        ("wallet_from_swipe",      c_wallet_from_swipe),
        ("wifi_status_connected",  c_wifi_status_connected),
        ("wifi_scan_nonempty",     c_wifi_scan_nonempty),
        ("wallet_pubkey_valid",    c_wallet_pubkey_valid),
        ("wallet_balance_matches", lambda d: c_wallet_balance_matches(d, args.rpc)),
        ("ai_ping",                c_ai_ping),
        *[
            (f"x402_{label}", _make_x402_case(label, url, lo, hi))
            for (label, url, lo, hi) in X402_URLS
        ],
        # payapi + spending harness cases (Task 17)
        ("payapi_catalog_refresh",          c_payapi_catalog_refresh),
        ("payapi_provider_refresh",         c_payapi_provider_refresh),
        ("payapi_guard_auto",               c_payapi_guard_auto),
        ("payapi_guard_refuse_over_confirm", c_payapi_guard_refuse_over_confirm),
        ("spend_today",                     c_spend_today),
        ("spend_add",                       c_spend_add),
        *([
            ("spending_endpoints_round_trip", _make_spending_http_case(args.ip)),
          ] if args.ip else []),
    ]

    dev.begin()
    print("-> TEST BEGIN\n")
    try:
        passed = failed = skipped = 0

        # Bracket the case loop with a heap reading so heap_no_leak can
        # flag long-running leaks that wouldn't show up in any single
        # case. It's inline (not a case in the list) because it depends
        # on every earlier case running first.
        heap_before = int(dev.send("TEST HEAP", timeout=2.0)[0].split()[3])

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

        # heap_no_leak: full-suite free-heap delta. Budget ±20 KB; bigger
        # drift typically means an unfreed String or JsonDocument. --only
        # filters skip this because partial runs won't hit a leak path.
        heap_after = int(dev.send("TEST HEAP", timeout=2.0)[0].split()[3])
        delta = heap_after - heap_before
        if args.only is None:
            if abs(delta) < 20 * 1024:
                print(f"[PASS] {'heap_no_leak':<26}  (delta {delta:+d} B)")
                passed += 1
            else:
                print(f"[FAIL] {'heap_no_leak':<26}  (delta {delta:+d} B; budget +/-20 KB)")
                failed += 1
        else:
            print(f"[SKIP] {'heap_no_leak':<26}  (--only filter active)")
            skipped += 1

        print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
        return 0 if failed == 0 else 1
    finally:
        dev.end()
        print("<- TEST END")


if __name__ == "__main__":
    sys.exit(main())
