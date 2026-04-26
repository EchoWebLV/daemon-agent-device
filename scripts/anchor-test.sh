#!/usr/bin/env bash
# Wrapper for `anchor test` — builds via scripts/anchor-build.sh first
# (which handles the toolchain quirks documented there), then runs
# `anchor test --skip-build` so anchor doesn't try to rebuild and trip
# over the IDL-build edge case.
#
# Pass extra args through to the test script, e.g.
#   ./scripts/anchor-test.sh -- --grep "vault_execute"
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

export PATH="$HOME/.avm/bin:$PATH"

"$(dirname "$0")/anchor-build.sh"

echo "==> anchor test --skip-build $*"
anchor test --skip-build "$@"
