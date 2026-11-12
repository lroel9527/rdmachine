#pragma once
// The server's macOS user interface: an icon in the menu bar (status,
// pairing codes, fingerprint, disconnect, permissions, settings) and a
// floating statistics panel for debugging. The server itself is
// ServerRuntime; this file only shows and edits it.
#include <string>

#include "server_runtime.h"

namespace rdm {

struct MenuBarAppInfo {
  std::string fingerprint;  // formatted
  std::string user;         // the account clients log in as
  std::string version;
};

// Runs the AppKit main loop until Quit, SIGINT or SIGTERM, then returns
// (the caller stops the runtime).
void RunMenuBarApp(ServerRuntime& runtime, const MenuBarAppInfo& info);

// --headless: no menu bar icon or window, but still AppKit's main loop until
// SIGINT or SIGTERM. A process only follows display changes (a virtual
// desktop built for a client, a new resolution) through that loop; with a
// bare run loop the new display's size and scale stay unknown to it.
void RunHeadlessApp();

// Whether another copy of this application already runs in this login
// session (started by the LaunchAgent and again as a login item or from the
// Finder, say): the second one should leave instead of failing on the port.
bool AnotherInstanceRunning();

// A modal alert for a server that could not start (no menu bar icon yet).
void ShowStartupError(const std::string& message);

}  // namespace rdm
