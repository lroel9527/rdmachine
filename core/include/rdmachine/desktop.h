#pragma once
// The desktop a client asks for ("请求桌面", docs/protocol.md): its size in
// pixels and how many pixels make a point (the HiDPI scale). The server
// builds a virtual display of exactly that shape, so one pixel of the
// picture is one pixel of the client's window and nothing is scaled on the
// way: the client's Retina window gets a Retina desktop, whatever screen the
// server has (or has none).
//
// Platform independent: the message and what a server makes of a request.
// Building the display is the session library's (VirtualDesktop).
#include <cstdint>
#include <optional>
#include <string>

namespace rdm {

// rdaccel application message type (RDA_MSG_USER_FIRST + 3), client to
// server: the client's window changed, build the desktop anew.
constexpr uint32_t kDesktopRequestMessageType = 0x10003;

struct DesktopSpec {
  int32_t width = 0;   // pixels
  int32_t height = 0;  // pixels
  double scale = 1.0;  // pixels per point: 2 on a Retina screen

  bool operator==(const DesktopSpec&) const = default;
};

// Limits of what a server builds.
inline constexpr int32_t kMinDesktopPoints = 320;   // either side, in points
inline constexpr int32_t kMaxDesktopWidth = 7680;   // pixels
inline constexpr int32_t kMaxDesktopHeight = 4320;  // pixels

// What a server builds for `wanted`, or nullopt if it makes no sense (no
// size, a scale that is not a positive number). The scale becomes 1 or 2
// (macOS has no others; 1.5 and up count as 2), the size is brought within
// the limits and made a whole number of points with even pixel sizes (video
// needs even sizes). The pixels stay as asked wherever possible: a pixel
// for a pixel matters more than the exact size of a point.
std::optional<DesktopSpec> PlanDesktop(const DesktopSpec& wanted);

// The size in points of a planned desktop.
inline int32_t DesktopPointsWide(const DesktopSpec& d) { return d.width / (d.scale >= 1.5 ? 2 : 1); }
inline int32_t DesktopPointsHigh(const DesktopSpec& d) { return d.height / (d.scale >= 1.5 ? 2 : 1); }

// JSON: {"width":2880,"height":1800,"scale":2}
std::string EncodeDesktopSpec(const DesktopSpec& d);
// nullopt for anything that is not such an object with a usable size.
std::optional<DesktopSpec> DecodeDesktopSpec(const std::string& payload);

// "2880x1800@2"; "WxH" alone is scale 1. For command lines and logs.
std::string FormatDesktopSpec(const DesktopSpec& d);
std::optional<DesktopSpec> ParseDesktopSpec(const std::string& text);

}  // namespace rdm
