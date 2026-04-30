#!/bin/sh
# Daemon → Claude Code one-shot installer.
#
# Downloads the MCP server from the device and registers it with Claude
# Code in one step. The web UI builds the user-facing command as:
#
#   curl -fsSL http://<host>/install.sh | sh -s <host>
#
# so $1 always holds the hostname/IP the user reached the device on.
# Falls through to daemon.local for users who curl this directly.

set -e

HOST="${1:-${DAEMON_HOST:-daemon.local}}"
DEST="$HOME/.daemon"
SCRIPT="$DEST/mcp.mjs"

echo "Daemon → Claude Code installer"
echo "  host: $HOST"

if ! command -v claude >/dev/null 2>&1; then
  echo "✗ 'claude' CLI not found on PATH." >&2
  echo "  Install Claude Code first: https://claude.com/code" >&2
  exit 1
fi

if ! command -v node >/dev/null 2>&1; then
  echo "✗ 'node' not found. Install Node 18+ first." >&2
  exit 1
fi

mkdir -p "$DEST"

echo "→ Downloading MCP server…"
curl -fsSL "http://$HOST/mcp.mjs" -o "$SCRIPT"

echo "→ Registering with Claude Code…"
# Idempotent: drop any prior 'daemon' registration so re-runs work cleanly.
claude mcp remove daemon >/dev/null 2>&1 || true
claude mcp add daemon -- node "$SCRIPT" --ip "$HOST"

echo
echo "✓ Done. Restart Claude Code, then run:  claude mcp list"
