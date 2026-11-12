#!/bin/sh
# Experiment 1 (docs/decisions.md, "登录窗口和锁屏"): run the installed
# RDMachine Server as a LaunchAgent in the login window session, to see
# whether it can capture the login window and type into it.
#
#   sudo tools/loginwindow-experiment/install.sh     # then log out
#   sudo tools/loginwindow-experiment/uninstall.sh   # when done
#
# The agent only loads in the login window session (LimitLoadToSessionType
# LoginWindow), so nothing changes until you log out. It listens on port
# 4200 (the normal server keeps 4100) with its own identity in
# /var/tmp/rdmachine-lw, so the client asks to trust a new fingerprint for
# HOST:4200. The pairing code and all messages go to
# /var/tmp/rdmachine-lw/server.log (read it over SSH from the other Mac).
set -eu

LABEL=org.rdmachine.loginwindow-experiment
PLIST=/Library/LaunchAgents/$LABEL.plist
DIR=/var/tmp/rdmachine-lw
SERVER="/Applications/RDMachine Server.app/Contents/MacOS/rdmachine-server"

if [ "$(id -u)" -ne 0 ]; then
  echo "run with sudo" >&2
  exit 1
fi
if [ ! -x "$SERVER" ]; then
  echo "not found: $SERVER" >&2
  exit 1
fi

mkdir -p "$DIR"
chmod 755 "$DIR"

# Runs in the login window session: notes who and where it runs, then
# becomes the server (exec keeps the server's own code identity for the
# Screen Recording and Accessibility permissions).
cat > "$DIR/run.sh" <<'EOF'
#!/bin/sh
{
  echo "=== $(date '+%Y-%m-%d %H:%M:%S') login window agent starting"
  echo "id: $(id)"
  echo "console owner: $(stat -f %Su /dev/console)"
  echo "launchd session: $(launchctl managername 2>/dev/null || echo ?)"
} >&2
exec "/Applications/RDMachine Server.app/Contents/MacOS/rdmachine-server" \
  --headless --pairing-code --port=4200 --config-dir=/var/tmp/rdmachine-lw/config
EOF
chmod 755 "$DIR/run.sh"

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>AssociatedBundleIdentifiers</key><array><string>org.rdmachine.server</string></array>
  <key>ProcessType</key><string>Interactive</string>
  <key>LimitLoadToSessionType</key><array><string>LoginWindow</string></array>
  <key>ProgramArguments</key><array><string>/bin/sh</string><string>$DIR/run.sh</string></array>
  <key>RunAtLoad</key><true/>
  <key>StandardOutPath</key><string>$DIR/server.log</string>
  <key>StandardErrorPath</key><string>$DIR/server.log</string>
</dict>
</plist>
EOF
chown root:wheel "$PLIST"
chmod 644 "$PLIST"
plutil -lint "$PLIST"
echo "installed $PLIST; log out to start the experiment, log: $DIR/server.log"
