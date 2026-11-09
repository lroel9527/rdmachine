#pragma once
// The server's own settings, as the menu bar app edits them:
// <config dir>/server-settings.json. Command-line options override them for
// one run. Unknown keys are ignored and missing ones keep their defaults, so
// older and newer versions can share the file.
#include <cstdint>
#include <string>

#include "rdmachine/clipboard_sync.h"
#include "rdmachine/protocol.h"

namespace rdm {

struct ServerSettings {
  uint16_t port = kDefaultPort;  // control TCP and media UDP; TCP fallback on port + 1
  bool offer_pairing_codes = true;
  bool view_only = false;
  ClipboardMode clipboard_mode = ClipboardMode::kBoth;
  int32_t fps = 0;           // 0 = rdaccel's default
  int32_t bitrate_kbps = 0;  // 0 = rdaccel's default

  bool operator==(const ServerSettings&) const = default;
};

std::string ServerSettingsPath(const std::string& config_dir);

// Defaults if the file does not exist. A file that is not valid JSON fails
// (so it is not silently overwritten); out-of-range values fall back to the
// defaults.
bool LoadServerSettings(const std::string& config_dir, ServerSettings* out, std::string* error);
bool SaveServerSettings(const std::string& config_dir, const ServerSettings& s, std::string* error);

}  // namespace rdm
