#!/usr/bin/env bash
# Deploy agent_program to Solana devnet.
#
# Prereqs (one-time):
#   solana config set --url devnet
#   solana balance      # need ≥ 3 SOL on devnet (airdrop a few times if needed)
#
# This script:
#   1. Builds via scripts/anchor-build.sh (handles toolchain quirks)
#   2. Deploys to devnet using the program-keypair-derived ID
#   3. Updates Anchor.toml's [programs.devnet] entry to match
#   4. Verifies with `solana program show`
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

export PATH="$HOME/.avm/bin:$PATH"

"$(dirname "$0")/anchor-build.sh"

PROG_ID=$(solana-keygen pubkey target/deploy/agent_program-keypair.json)
echo "==> program id: $PROG_ID"

echo "==> anchor deploy --provider.cluster devnet"
anchor deploy --provider.cluster devnet

# Make Anchor.toml's [programs.devnet] line match the deployed id (idempotent)
sed -i.bak -E "/^\[programs\.devnet\]/,/^\[/{ s|^agent_program = \".*\"|agent_program = \"$PROG_ID\"|; }" Anchor.toml
rm -f Anchor.toml.bak

echo "==> verifying with solana program show"
solana program show "$PROG_ID" --url devnet
