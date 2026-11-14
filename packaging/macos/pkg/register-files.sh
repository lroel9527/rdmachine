#!/bin/sh
# Installer helper (root): registers the app's File Provider domain for the
# logged-in user, so the RDMachine switch is in System Settings > General >
# Login Items & Extensions right after installing (docs/file-paste.md).
# The domain has to be added by the app itself, as that user.
#   register-files.sh APP EXECUTABLE
APP=$1
EXE=$2
USER_NAME=$(stat -f %Su /dev/console)
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || exit 0  # no one logged in: the first paste adds it
USER_ID=$(id -u "$USER_NAME") || exit 0
as_user() { launchctl asuser "$USER_ID" sudo -u "$USER_NAME" "$@"; }
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
as_user "$LSREGISTER" -f -R "$APP" >/dev/null 2>&1
as_user pluginkit -a "$APP/Contents/PlugIns/RDMachine Files.appex" >/dev/null 2>&1
# The extension may take a moment to be known after an install.
ok=0
for i in 1 2 3 4 5; do
  if as_user "$APP/Contents/MacOS/$EXE" --register-files; then ok=1; break; fi
  sleep 2
done
[ "$ok" = 1 ] || echo "rdmachine postinstall: could not register the RDMachine location (the first paste will)" >&2
# The switch can only be turned on by the user (without a developer
# account there is no way around that): open the page it is on. Both
# components may run this; the page opens once either way.
as_user open "x-apple.systempreferences:com.apple.LoginItems-Settings.extension" >/dev/null 2>&1
exit 0
