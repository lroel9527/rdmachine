#pragma once
// This Mac's input source and its icon (docs/ime.md, 7.1.0): for the
// server's menu bar icon, which shows the input source the client types
// with. Shared by the window client and rdmachine-connect. macOS.
#include <cstdint>
#include <string>
#include <vector>

namespace rdmclient {

// The input source this Mac types with now (Text Input Sources id).
std::string CurrentInputSourceId();
// That input source's icon as the server's menu bar shows it: what macOS
// shows in its own menu bar, drawn at 32x32 pixels as a template (its shapes
// opaque black, the rest clear), PNG. Empty when it has none (or it would be
// larger than the server takes).
std::vector<uint8_t> InputSourceIconPng(const std::string& source_id);

}  // namespace rdmclient
