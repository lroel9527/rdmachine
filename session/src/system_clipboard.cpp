#include "rdmachine/session/system_clipboard.h"

#include <cstdlib>

namespace rdm {

// Non-Apple builds; macOS uses system_clipboard_mac.mm.
std::shared_ptr<Clipboard> SystemClipboard(const std::string&) { return nullptr; }

std::string ClipboardStagingDir() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) return std::string(xdg) + "/rdmachine/clipboard";
  if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/.cache/rdmachine/clipboard";
  return {};
}

}  // namespace rdm
