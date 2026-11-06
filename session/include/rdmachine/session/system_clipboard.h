#pragma once
// This computer's clipboard, for ClipboardSync (core/clipboard_sync.h).
#include <memory>

#include "rdmachine/clipboard_sync.h"

namespace rdm {

// macOS: the general NSPasteboard (plain text only; skips content that
// password managers mark org.nspasteboard.ConcealedType or TransientType).
// Other systems: null for now, so the clipboard is not shared (phase 1 is
// Mac to Mac; the X11/Wayland clipboard comes with Linux support).
std::shared_ptr<Clipboard> SystemClipboard();

}  // namespace rdm
