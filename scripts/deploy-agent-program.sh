#!/usr/bin/env bash
# Deploy agent_program to Solana mainnet OR devnet.
#
# Usage:
#   ./scripts/deploy-agent-program.sh devnet
#   ./scripts/deploy-agent-program.sh mainnet
#
# Mainnet path uses the dedicated deployer keypair under .secrets/ (gitignored)
# so we never accidentally bill the regular CLI wallet for an upgrade. Devnet
# path uses ~/.config/solana/id.json — the CLI default.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

CLUSTER="${1:-devnet}"
case "$CLUSTER" in
    devnet)
        ANCHOR_CLUSTER="devnet"
        SHOW_URL="devnet"
        WALLET_FLAG=""
        ANCHOR_TOML_KEY="programs.devnet"
        ;;
    mainnet|mainnet-beta)
        ANCHOR_CLUSTER="mainnet"
        SHOW_URL="mainnet-beta"
        WALLET_FLAG="--provider.wallet $(pwd)/.secrets/agent-deployer.json"
        ANCHOR_TOML_KEY="programs.mainnet"
        ;;
    *)
        echo "usage: $0 {devnet|mainnet}" >&2
        exit 2
        ;;
esac

export PATH="$HOME/.avm/bin:$PATH"

"$(dirname "$0")/anchor-build.sh"

PROG_ID=$(solana-keygen pubkey target/deploy/agent_program-keypair.json)
echo "==> program id:    $PROG_ID"
echo "==> cluster:       $ANCHOR_CLUSTER"
echo "==> wallet:        ${WALLET_FLAG:-default ~/.config/solana/id.json}"

echo "==> anchor deploy --provider.cluster $ANCHOR_CLUSTER $WALLET_FLAG"
anchor deploy --provider.cluster "$ANCHOR_CLUSTER" $WALLET_FLAG

# Make Anchor.toml's matching line agree with the deployed id (idempotent).
# We escape the section name's "." for the sed bracket-expression:
SECTION_RE=$(printf '%s\n' "$ANCHOR_TOML_KEY" | sed 's/\./\\./g')
sed -i.bak -E "/^\[$SECTION_RE\]/,/^\[/{ s|^agent_program = \".*\"|agent_program = \"$PROG_ID\"|; }" Anchor.toml
rm -f Anchor.toml.bak

echo "==> verifying with solana program show"
solana program show "$PROG_ID" --url "$SHOW_URL"
