#pragma once
// Control protocol constants (docs/protocol.md).
#include <cstddef>
#include <cstdint>

namespace rdm {

// The base protocol: frozen at 0. Both sides must have the same base; it
// changes only for a change an older side cannot follow (none is planned).
inline constexpr int kProtocolVersion = 0;
// What this build adds on top of the base, compatibly. Each side says its
// revision in its hello (absent: 0) and both use the lower one; a feature of
// revision N is used only when the negotiated revision is at least N, and an
// older side simply does without it. Fields and messages a revision adds
// are optional, so an older side ignores them.
//   0  everything up to rdmachine 3.4 (optional fields since then: desktop,
//      text_mode, ...)
//   1  the revision itself (rdmachine 4.0)
//   2  extended desktops: session_request.extra_desktops and
//      session_ready.extra_displays (rdmachine 4.0)
inline constexpr int kProtocolRevision = 2;
inline constexpr int NegotiatedRevision(int a, int b) { return a < b ? a : b; }

inline constexpr uint16_t kDefaultPort = 4100;  // TCP control (TLS) and UDP media

inline constexpr size_t kSessionKeyBytes = 32;  // rdaccel's key size
inline constexpr size_t kAuthTokenBytes = 32;   // one-time token for rdaccel's on_auth

const char* VersionString();

}  // namespace rdm
