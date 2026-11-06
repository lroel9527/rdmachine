#pragma once
// Cryptographically secure random bytes (from the OS through Mbed TLS / PSA).
#include <cstdint>
#include <span>

namespace rdm {

// Returns false only if the system random source fails; callers must then
// refuse to continue (never fall back to a weaker source).
[[nodiscard]] bool RandomBytes(std::span<uint8_t> out);

}  // namespace rdm
