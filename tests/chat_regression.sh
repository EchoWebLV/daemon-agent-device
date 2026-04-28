#!/usr/bin/env bash
# ----------------------------------------------------------------------------
# tests/chat_regression.sh — fast post-change regression for the chat path.
#
# Run after any major work that could regress the daemon-x402s plumbing,
# the firmware's tool dispatch, or the AI SDK / tool registry. Hits the
# device's POST /say endpoint directly (bypassing STT, so a flaky mic
# or empty Whisper/Deepgram doesn't mask a chat regression).
#
# What's covered:
#
#   1. chat_basic       — single-turn greeting. Verifies the wire is up
#                         end-to-end (firmware → /api/<provider> → model
#                         → reply → /say JSON).
#   2. chat_web_search  — current-events question that requires web_search
#                         + freshness over training memory. Looks for a
#                         specific recent fact in the reply.
#   3. swap             — "swap 0.01 USDC for SOL". Triggers the swap
#                         modal on the device (you must approve / cancel
#                         from the screen). Verifies the model invoked
#                         swap_tokens and the device's tool loop ran.
#   4. x402_service     — trending-tokens query that fires the user's
#                         registered CoinGecko x402 service via tool_calls.
#                         Verifies body.tools forwarding + x402 paid-call
#                         path through the device's execute_tool().
#
# Usage:
#
#   tests/chat_regression.sh                # default host: 192.168.100.7
#   DEVICE_HOST=192.168.x.y tests/chat_regression.sh
#   tests/chat_regression.sh swap           # run a single named test
#
# Exit code: 0 if everything passed, 1 if any check failed.
# ----------------------------------------------------------------------------
set -uo pipefail

DEVICE_HOST="${DEVICE_HOST:-192.168.100.7}"
SAY_URL="http://${DEVICE_HOST}/say"
SAY_TIMEOUT_S=120

# --- helpers ---------------------------------------------------------------

if [ -t 1 ]; then
  C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_BOLD=$'\033[1m'
  C_DIM=$'\033[2m';   C_RESET=$'\033[0m'
else
  C_GREEN=""; C_RED=""; C_BOLD=""; C_DIM=""; C_RESET=""
fi

PASS_COUNT=0
FAIL_COUNT=0
FAILED_NAMES=()

# Send a prompt to /say and emit just the reply text (or empty on error).
# Stderr stays clean so callers can capture the reply with $(...).
say() {
  local prompt="$1"
  local body
  body=$(printf '{"prompt":%s}' "$(printf '%s' "$prompt" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')")
  curl -s --max-time "$SAY_TIMEOUT_S" \
       -H 'content-type: application/json' \
       -d "$body" \
       "$SAY_URL" \
    | python3 -c 'import json,sys
try:
    d=json.load(sys.stdin)
    print(d.get("reply",""))
except Exception:
    pass'
}

# Run a named test. Args: name, prompt, "expect substring" (case-insensitive,
# pipe-separated alternatives — any one match is enough).
check() {
  local name="$1"
  local prompt="$2"
  local expect="$3"

  printf '%s%s%s\n' "$C_BOLD" "▶ ${name}" "$C_RESET"
  printf '%s  prompt:%s %s\n' "$C_DIM" "$C_RESET" "$prompt"

  local t0 t1 reply
  t0=$(python3 -c 'import time; print(time.time())')
  reply=$(say "$prompt")
  t1=$(python3 -c 'import time; print(time.time())')
  local dt
  dt=$(python3 -c "print(f'{($t1)-($t0):.1f}')")

  if [ -z "$reply" ]; then
    printf '%s  ✗ FAIL%s — empty reply (device offline? STT failure on text path?) [%ss]\n\n' \
           "$C_RED" "$C_RESET" "$dt"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    FAILED_NAMES+=("$name")
    return 1
  fi

  printf '%s  reply:%s  %s\n' "$C_DIM" "$C_RESET" "$reply"

  # Match any one of the pipe-separated alternatives, case-insensitive.
  local match=0
  IFS='|' read -r -a alts <<< "$expect"
  local lower_reply
  lower_reply=$(printf '%s' "$reply" | tr '[:upper:]' '[:lower:]')
  for alt in "${alts[@]}"; do
    local lower_alt
    lower_alt=$(printf '%s' "$alt" | tr '[:upper:]' '[:lower:]')
    if [[ "$lower_reply" == *"$lower_alt"* ]]; then
      match=1
      break
    fi
  done

  if [ "$match" -eq 1 ]; then
    printf '%s  ✓ PASS%s [%ss]\n\n' "$C_GREEN" "$C_RESET" "$dt"
    PASS_COUNT=$((PASS_COUNT + 1))
  else
    printf '%s  ✗ FAIL%s — expected one of: %s [%ss]\n\n' \
           "$C_RED" "$C_RESET" "$expect" "$dt"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    FAILED_NAMES+=("$name")
  fi
}

# --- tests -----------------------------------------------------------------

t_chat_basic() {
  check "chat_basic" \
        "Say hi back in exactly two words." \
        "hi|hello|hey|sup|yo"
}

t_chat_web_search() {
  # Fact that contradicts pre-2026 training memory — only correct if the
  # web_search tool actually fired and the freshness assertion overrode
  # the model's prior. As of 2026 the president is Iliana Iotova (Radev
  # resigned in January 2026).
  check "chat_web_search" \
        "Use web_search. Who is the current president of Bulgaria?" \
        "iotova|yotova"
}

t_swap() {
  printf '%s%s%s — needs a human at the device to approve/cancel the swap modal.\n' \
         "$C_BOLD" "⚠  swap" "$C_RESET"
  check "swap" \
        "Swap 0.01 USDC for SOL right now. Just do it." \
        "swap|usdc|sol|cancel|tx|received|approve"
}

t_x402_service() {
  # Hits the user's registered Trending x402 service (registry.frames.ag).
  # The model should call the trending tool which the device runs via
  # x402_post(); the reply names actual trending tokens.
  check "x402_service" \
        "What crypto tokens are trending right now? Use the trending tool." \
        "bitcoin|ethereum|solana|trending|btc|eth|sol|xrp|doge"
}

# --- runner ----------------------------------------------------------------

main() {
  printf '%schat regression %s%s @ %s%s\n\n' \
         "$C_BOLD" "→" "$C_RESET" "$DEVICE_HOST" "$C_RESET"

  if ! curl -s --max-time 5 -o /dev/null -w '' "http://${DEVICE_HOST}/state"; then
    printf '%s✗ device unreachable at %s%s — abort\n' "$C_RED" "$DEVICE_HOST" "$C_RESET"
    exit 1
  fi

  local only="${1:-}"
  case "$only" in
    "")              t_chat_basic; t_chat_web_search; t_swap; t_x402_service ;;
    chat_basic)      t_chat_basic ;;
    chat_web_search) t_chat_web_search ;;
    swap)            t_swap ;;
    x402_service)    t_x402_service ;;
    -h|--help)
      sed -n '/^# tests\/chat_regression/,/^# Exit code:/p' "$0" | sed 's/^# *//'
      exit 0
      ;;
    *)
      printf 'unknown test: %s\n' "$only" >&2
      exit 2
      ;;
  esac

  local total=$((PASS_COUNT + FAIL_COUNT))
  if [ "$FAIL_COUNT" -eq 0 ]; then
    printf '%s%s%d/%d passed%s\n' "$C_BOLD" "$C_GREEN" "$PASS_COUNT" "$total" "$C_RESET"
    exit 0
  else
    printf '%s%s%d/%d passed — failed: %s%s\n' \
           "$C_BOLD" "$C_RED" "$PASS_COUNT" "$total" \
           "${FAILED_NAMES[*]}" "$C_RESET"
    exit 1
  fi
}

main "$@"
