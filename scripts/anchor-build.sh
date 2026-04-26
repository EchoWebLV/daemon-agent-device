#!/usr/bin/env bash
# Wrapper for `anchor build` that handles two toolchain quirks in this repo:
#   1. Solana platform-tools v1.48 (the default bundled with anchor-cli) ships
#      cargo 1.84, which doesn't speak edition2024 — so the SBF build needs
#      `--tools-version v1.53` (which bundles cargo 1.89).
#   2. Anchor's combined `anchor build` forwards `-- --tools-version` to BOTH
#      the SBF stage AND the IDL stage's `cargo test`, which doesn't recognize
#      it. So we run the two stages separately.
#
# Anchor itself comes from avm (~/.avm/bin/anchor → 0.32.1) rather than the
# legacy 0.30.1 in ~/.cargo/bin. The PATH bump below makes that explicit.
#
# Output: target/deploy/agent_program.so plus target/idl/*.json and
# target/types/*.ts for every program in the workspace.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

export PATH="$HOME/.avm/bin:$PATH"

ANCHOR=$(command -v anchor)
echo "==> using $ANCHOR ($(anchor --version))"

echo "==> seeding committed program keypair into target/deploy"
mkdir -p target/deploy
for crate in programs/*/; do
  name=$(basename "$crate")
  src="$crate/keypair.json"
  if [[ -f "$src" ]]; then
    cp -f "$src" "target/deploy/${name}-keypair.json"
    chmod 600 "target/deploy/${name}-keypair.json"
  fi
done

echo "==> SBF build (--no-idl -- --tools-version v1.53)"
anchor build --no-idl -- --tools-version v1.53

echo "==> IDL build"
mkdir -p target/idl target/types
for crate in programs/*/; do
  name=$(basename "$crate")
  anchor idl build -p "$name" \
    -o "target/idl/${name}.json" \
    -t "target/types/${name}.ts"
  echo "    wrote target/idl/${name}.json"
  echo "    wrote target/types/${name}.ts"
done

echo "==> done"
