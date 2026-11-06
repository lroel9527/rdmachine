#include "rdmachine/session/system_clipboard.h"

namespace rdm {

// Non-Apple builds; macOS uses system_clipboard_mac.mm.
std::shared_ptr<Clipboard> SystemClipboard() { return nullptr; }

}  // namespace rdm
