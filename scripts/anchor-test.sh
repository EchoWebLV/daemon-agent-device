#!/usr/bin/env bash
# Wrapper for `anchor test` — builds via scripts/anchor-build.sh first
# (which handles the toolchain quirks documented there), then manually
# starts the local validator (solana-test-validator 2.3.x is incompatible
# with anchor 0.32.1's built-in startup detection), deploys the program,
# and finally runs `anchor test --skip-build --skip-local-validator
# --skip-deploy` so anchor only runs the test script.
#
# Pass extra args through to the test script, e.g.
#   ./scripts/anchor-test.sh -- --grep "vault_execute"
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

export PATH="$HOME/.avm/bin:$PATH"

"$(dirname "$0")/anchor-build.sh"

# Kill any leftover validator from a previous run
pkill -f "solana-test-validator" 2>/dev/null || true
sleep 1

WALLET="$HOME/.config/solana/id.json"
MINT=$(solana address --keypair "$WALLET")
PROGRAM_ID="FNVcw2kCnzSxZwqNEnoSMmVnTLXaiusxupLZ4CWkYPAA"
SO_PATH="target/deploy/agent_program.so"
LEDGER=".anchor/test-ledger"

rm -rf "$LEDGER"

echo "==> starting solana-test-validator"
solana-test-validator \
  --ledger "$LEDGER" \
  --mint "$MINT" \
  --bpf-program "$PROGRAM_ID" "$SO_PATH" \
  --bind-address 0.0.0.0 \
  --rpc-port 8899 \
  --quiet 2>/dev/null &
VALIDATOR_PID=$!

# Wait until the RPC is responsive
echo "==> waiting for validator..."
until curl -sf http://127.0.0.1:8899 \
    -X POST -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":1,"method":"getLatestBlockhash"}' \
    | grep -q '"blockhash"'; do
  sleep 1
done
echo "==> validator ready (pid=$VALIDATOR_PID)"

# Run mocha directly instead of via `anchor test` — anchor 0.32.1 overrides
# ANCHOR_PROVIDER_URL when it spawns the test script, which breaks AnchorProvider.env().
# Running mocha ourselves keeps our env vars intact.
export ANCHOR_PROVIDER_URL="http://127.0.0.1:8899"
export ANCHOR_WALLET="$WALLET"

# Replicate what Anchor.toml [scripts].test does, but pass through extra mocha args.
# The caller passes args after "--", e.g.:  ./anchor-test.sh -- --grep "foo"
# We strip the leading "--" so we can forward bare mocha flags.
if [[ "${1:-}" == "--" ]]; then
  shift
fi
echo "==> ts-mocha $*"
set +e
./node_modules/.bin/ts-mocha -p ./tsconfig.json -t 1000000 'tests/**/*.ts' "$@"
TEST_EXIT=$?
set -e

kill "$VALIDATOR_PID" 2>/dev/null || true
wait "$VALIDATOR_PID" 2>/dev/null || true

exit $TEST_EXIT
