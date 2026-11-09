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

// A modal alert for a server that could not start (no menu bar icon yet).
void ShowStartupError(const std::string& message);

}  // namespace rdm
