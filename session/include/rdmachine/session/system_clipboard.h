#pragma once
// This computer's clipboard, for ClipboardSync (core/clipboard_sync.h).
#include <memory>
#include <string>

#include "rdmachine/clipboard_sync.h"

namespace rdm {

// macOS: an NSPasteboard, the general one unless `pasteboard_name` names
// another (tests use a private one, so a running session does not share
// what they copy). Reads and writes text, RTF, HTML, images (as PNG) and
// copied files; skips content that password managers mark
// org.nspasteboard.ConcealedType or TransientType. Files it puts on the
// pasteboard are marked as downloaded (com.apple.quarantine), as they came
// from another computer.
// Other systems: null for now, so the clipboard is not shared (phase 1 is
// Mac to Mac; the X11/Wayland clipboard comes with Linux support).
std::shared_ptr<Clipboard> SystemClipboard(const std::string& pasteboard_name = {});

// Where files copied on the other computer are put before they are pasted
// (macOS: ~/Library/Caches/RDMachine/Clipboard; elsewhere
// $XDG_CACHE_HOME/rdmachine/clipboard).
std::string ClipboardStagingDir();

}  // namespace rdm
