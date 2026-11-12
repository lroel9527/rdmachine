#!/bin/sh
# Removes what install.sh added. Keeps /var/tmp/rdmachine-lw/server.log
# unless --all is given.
set -eu
LABEL=org.rdmachine.loginwindow-experiment
if [ "$(id -u)" -ne 0 ]; then
  echo "run with sudo" >&2
  exit 1
fi
rm -f "/Library/LaunchAgents/$LABEL.plist"
pkill -f 'rdmachine-server --headless --pairing-code --port=4200' 2>/dev/null || true
if [ "${1:-}" = "--all" ]; then
  rm -rf /var/tmp/rdmachine-lw
else
  rm -rf /var/tmp/rdmachine-lw/config /var/tmp/rdmachine-lw/run.sh
fi
echo "removed $LABEL"
