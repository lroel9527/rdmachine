#pragma once
// This computer's clipboard, for ClipboardSync (core/clipboard_sync.h).
#include <memory>
#include <string>

#include "rdmachine/clipboard_sync.h"

namespace rdm {

// macOS: an NSPasteboard, the general one unless `pasteboard_name` names
// another (tests use a private one, so a running session does not share
// what they copy). Reads and writes text, RTF, HTML and images (as PNG);
// files copied in Finder are read as their names only. Skips content that
// password managers mark org.nspasteboard.ConcealedType or TransientType.
// Other systems: null for now, so the clipboard is not shared (phase 1 is
// Mac to Mac; the X11/Wayland clipboard comes with Linux support).
std::shared_ptr<Clipboard> SystemClipboard(const std::string& pasteboard_name = {});

}  // namespace rdm
