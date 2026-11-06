#pragma once
// Standard base64 (RFC 4648, with padding) for binary fields in control
// messages, such as the session key and the one-time auth token.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rdm {

std::string Base64Encode(std::span<const uint8_t> bytes);
// nullopt on any character outside the alphabet, bad padding or bad length.
std::optional<std::vector<uint8_t>> Base64Decode(std::string_view text);

}  // namespace rdm
