#include "rdmachine/session/system_clipboard.h"

namespace rdm {

// Non-Apple builds; macOS uses system_clipboard_mac.mm.
std::shared_ptr<Clipboard> SystemClipboard(const std::string&) { return nullptr; }

}  // namespace rdm
