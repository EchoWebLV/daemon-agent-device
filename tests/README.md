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

## Protocol

See the design doc at
`docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md` and the
firmware implementation at `src/testharness.cpp`.
