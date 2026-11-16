#!/usr/bin/env bash
# Runs the real server and command-line client against each other on this
# machine: first connection with a pairing code (trusting the new
# fingerprint), then checks that the fingerprint was pinned and that a used
# pairing code is refused. Needs a display to capture ($DISPLAY on Linux).
# With RDM_TEST_VIRTUAL_DESKTOP=1 (macOS) the first connection also asks for
# a HiDPI desktop and then another size: the server builds a virtual display,
# which rearranges this Mac's displays for a few seconds.
# The server runs standalone (never as an agent of a daemon installed on
# this Mac) and sends the computed test sound (--audio-source=test, no
# device); the client asks for sound and takes it on a null device: the
# first connection must have received sound packets.
#
#   tools/smoke_test.sh BUILD_BIN_DIR
set -euo pipefail
bin=${1:?usage: smoke_test.sh BUILD_BIN_DIR}
dir=$(mktemp -d)
port=$((43000 + RANDOM % 2000))
trap 'kill $server_pid 2>/dev/null || true; rm -rf "$dir"' EXIT

# stdin stays open (the server reads Enter for new pairing codes).
sleep 120 | "$bin/rdmachine-server" --standalone --port=$port --pairing-code --view-only --headless --clipboard=off \
  --audio-source=test --config-dir="$dir/server" \
  >"$dir/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 50); do grep -q "pairing code:" "$dir/server.log" 2>/dev/null && break; sleep 0.1; done
cat "$dir/server.log"
code=$(grep -o "pairing code: [0-9]*" "$dir/server.log" | head -1 | awk '{print $3}')

export RDMACHINE_CONFIG_DIR="$dir/client"
desktop=()
if [ -n "${RDM_TEST_VIRTUAL_DESKTOP:-}" ]; then
  desktop=(--desktop=2880x1800@2 --resize-desktop=2048x1536@2)
fi
"$bin/rdmachine-connect" 127.0.0.1:$port --code="$code" --accept-new-host --frames=30 --seconds=20 --audio --clipboard=off \
  ${desktop[@]+"${desktop[@]}"} | tee "$dir/first.log"
packets=$(sed -n 's/^sound: on, \([0-9]*\) packets.*/\1/p' "$dir/first.log")
[ -n "$packets" ] && [ "$packets" -gt 0 ] || { echo "no sound arrived"; cat "$dir/server.log"; exit 1; }
if [ -n "${RDM_TEST_VIRTUAL_DESKTOP:-}" ]; then
  grep -q "desktop built: 2880x1800@2" "$dir/first.log" || { echo "no desktop was built"; cat "$dir/server.log"; exit 1; }
  grep -q "the picture is now 2048x1536$" "$dir/first.log" || { echo "the desktop was not resized"; exit 1; }
fi

expected=$("$bin/rdmachine-server" --standalone --config-dir="$dir/server" --fingerprint | tr -d ' ')
grep -q "127.0.0.1:$port sha256:$expected" "$dir/client/known_hosts" || {
  echo "fingerprint was not pinned"; cat "$dir/client/known_hosts"; exit 1; }

if "$bin/rdmachine-connect" 127.0.0.1:$port --code="$code" --seconds=5 2>"$dir/second.log"; then
  echo "a used pairing code was accepted"; exit 1
fi
grep -q code_expired "$dir/second.log" || { cat "$dir/second.log"; exit 1; }
echo "smoke test passed"
