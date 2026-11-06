#pragma once
// Control protocol constants (docs/protocol.md).
#include <cstddef>
#include <cstdint>

namespace rdm {

// 0 while the protocol is unstable (M0). Both sides must match exactly until
// version 1; from 1 on, a side accepts any version >= its minimum.
inline constexpr int kProtocolVersion = 0;

inline constexpr uint16_t kDefaultPort = 4100;  // TCP control (TLS) and UDP media

inline constexpr size_t kSessionKeyBytes = 32;  // rdaccel's key size
inline constexpr size_t kAuthTokenBytes = 32;   // one-time token for rdaccel's on_auth

const char* VersionString();

}  // namespace rdm
