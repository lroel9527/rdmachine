#include "rdmachine/session/system_clipboard.h"
#include "rdmachine/session/system_files.h"

namespace rdm {

// Non-Apple builds; macOS uses system_clipboard_mac.mm.
std::shared_ptr<Clipboard> SystemClipboard(const std::string&) { return nullptr; }
std::shared_ptr<FileTarget> SystemFileTarget() { return nullptr; }
bool RegisterFileDomain(std::string* error) {
  *error = "not on this system";
  return false;
}
std::string FileStagingDir() { return {}; }

}  // namespace rdm
