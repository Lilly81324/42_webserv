#!/usr/bin/env bash
# test_stalled_clients.sh

# Utiliy script for running a stalling Client
# !!! Dont run this directly, this is used in test_shutdown.sh !!!

# Sends stalled HTTP requests: send headers, sleep, then send body.
# Captures the response and checks for HTTP 503 status.
#
# Requirements: bash, nc (netcat), timeout, mktemp, head, grep
#
# Usage:
#   ./test_stalled_clients.sh [HOST] [PORT] [PATH] [DELAY] [BODY] [ITERATIONS] [OVERALL_TIMEOUT]
#
# Example:
#   ./test_stalled_clients.sh 127.0.0.1 8080 /upload 5 "Hello world\n" 3 30
#
# Default values:
HOST="127.0.0.1"
PORT="${1:-8080}"
TARGET="/upload"                    # leading '-' prevents empty param from being treated as option
DELAY="5"                   # seconds to sleep between headers and body
BODY="Hello world\n"        # body sent after the delay
ITER="1"                    # number of iterations
OVERALL_TIMEOUT="10"        # timeout (secs) for the whole netcat operation per request

# Normalize PATH if started with '-' default above
if [[ "$PATH" = --/ ]]; then PATH="/"; fi

# Tools check
command -v nc >/dev/null 2>&1 || { echo "ERROR: nc (netcat) not found in PATH."; exit 2; }
command -v timeout >/dev/null 2>&1 || { echo "ERROR: timeout command not found in PATH."; exit 2; }

# Compute content length of BODY (bytes)
# Use printf to interpret \n in default BODY
BODY_BYTES=$(printf "%b" "$BODY")
CONTENT_LENGTH=$(printf "%s" "$BODY_BYTES" | wc -c | tr -d ' ')

echo "Test stalled client: $ITER requests -> http://$HOST:$PORT$TARGET"
echo "Delay between headers/body: ${DELAY}s, Content-Length: ${CONTENT_LENGTH}, per-request timeout: ${OVERALL_TIMEOUT}s"
echo

fail_count=0
pass_count=0

send_and_check() {
  local i="$1"
  local tmpfile
  tmpfile=$(mktemp /tmp/stalled_response.XXXXXX) || { echo "mktemp failed"; exit 2; }

  # Build headers (interpret \r\n properly)
  local headers
  headers=$(printf "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %s\r\nConnection: close\r\n\r\n" \
           "$TARGET" "$HOST" "$CONTENT_LENGTH")

  echo "=== Request #$i ==="
  echo "Headers being sent (then sleep ${DELAY}s):"
  printf "%s" "$headers"
  echo "-----"
  echo "Body bytes to be sent after sleep:"
  printf "%b" "$BODY_BYTES"
  echo
  echo "Sending... (will sleep ${DELAY}s between header and body)"

  # Send headers, sleep, then send body. Capture server response to tmpfile.
  # timeout wraps nc to bound the whole operation (connect + wait for reply).
  # The '|| true' prevents script exit if timeout returns non-zero.
  ( printf "%s" "$headers"; sleep "$DELAY"; printf "%s" "$BODY_BYTES" ) \
    | timeout "$OVERALL_TIMEOUT" nc "$HOST" "$PORT" >"$tmpfile" 2>/dev/null || true

  # Inspect response (first line)
  if [[ ! -s "$tmpfile" ]]; then
    echo "NO RESPONSE (connection closed / timed out)."
    rm -f "$tmpfile"
    return 2
  fi

  # Read first non-empty line (remove trailing CR)
  status_line=$(grep -m1 -E '^HTTP' "$tmpfile" | tr -d '\r' || true)
  if [[ -z "$status_line" ]]; then
    # maybe server used lowercase or some other greeting, print head
    status_line=$(head -n 1 "$tmpfile" | tr -d '\r' || true)
  fi

  echo "Response status line: '$status_line'"
  if echo "$status_line" | grep -qE '^[[:space:]]*HTTP/[0-9.]+[[:space:]]+503[[:space:]]'; then
    echo "=> PASSED: got 503"
    pass_count=$((pass_count+1))
    rm -f "$tmpfile"
    return 0
  else
    echo "=> FAILED: did not get 503 (see full response below):"
    echo "---- response start ----"
    sed -n '1,200p' "$tmpfile"
    echo "---- response end ----"
    fail_count=$((fail_count+1))
    rm -f "$tmpfile"
    return 1
  fi
}

# Loop
for ((i=1;i<=ITER;i++)); do
  send_and_check "$i"
  ret=$?
  # ret 0 = pass, 1 = response but not 503, 2 = no response / timed out
  if [[ $ret -eq 2 ]]; then
    echo "Request #$i: no response or timed out (consider increasing OVERALL_TIMEOUT)."
    fail_count=$((fail_count+1))
  fi
  echo
done

echo "Summary: passed=$pass_count failed=$fail_count (out of $ITER)"

if [[ $fail_count -gt 0 ]]; then
  exit 1
else
  exit 0
fi
