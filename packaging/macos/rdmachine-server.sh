#!/bin/bash
# Starts "RDMachine Server.app" from Terminal, with the server's output and
# input (Enter for a new pairing code) in this window. Takes the same options
# as rdmachine-server, for example:
#
#   ./rdmachine-server.sh --pairing-code
#
# Why not run Contents/MacOS/rdmachine-server directly: macOS would then ask
# for Screen Recording on behalf of Terminal, not the server. Started through
# `open`, the app is its own process and the permissions are recorded against
# RDMachine Server and its signature, so they survive rebuilds signed with the
# same certificate. Ctrl-C stops the server.
#
# Looks for the app next to this script, then in /Applications and
# ~/Applications; RDMACHINE_SERVER_APP=/path/to/app overrides.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
app=${RDMACHINE_SERVER_APP:-}
if [ -z "$app" ]; then
  for c in "$here/RDMachine Server.app" "/Applications/RDMachine Server.app" "$HOME/Applications/RDMachine Server.app"; do
    if [ -d "$c" ]; then app=$c; break; fi
  done
fi
[ -n "$app" ] && [ -d "$app" ] || { echo "RDMachine Server.app not found (set RDMACHINE_SERVER_APP)" >&2; exit 1; }
exe="$app/Contents/MacOS/rdmachine-server"

# Options that only print something need no permissions: run them here.
for a in "$@"; do
  case "$a" in --version|--fingerprint|-h|--help) exec "$exe" "$@" ;; esac
done

tty=$(tty) || { echo "run this from a terminal window" >&2; exit 1; }
if pgrep -qf "^$exe" 2>/dev/null; then
  echo "RDMachine Server is already running (stop it with: pkill -f '$exe')" >&2
  exit 1
fi

stop() {
  pkill -TERM -f "^$exe" 2>/dev/null || true
}
trap 'stop; exit 130' INT
trap 'stop; exit 143' TERM HUP

# -n: a new instance even if one was just quitting; -W: wait until it exits.
open -n -W --stdin "$tty" --stdout "$tty" --stderr "$tty" "$app" --args "$@" &
wait $! || exit $?
