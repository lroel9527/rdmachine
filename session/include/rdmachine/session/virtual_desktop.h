#pragma once
// The desktop the server builds for a client's request (docs/protocol.md,
// "请求桌面"): a virtual display of exactly the requested pixel size and
// HiDPI scale, made the computer's one desktop for as long as it exists.
//
// On macOS it is a CGVirtualDisplay. It becomes the main display, which is
// rdaccel's display 0, and the computer's real displays mirror it, so every
// window is on it. Destroying it puts the real displays back as they were.
#include <functional>
#include <memory>
#include <string>

#include "rdmachine/desktop.h"

namespace rdm {

class VirtualDesktop {
 public:
  // Removes the display and restores the arrangement found when it was built.
  virtual ~VirtualDesktop() = default;

  // The desktop as it is now (a PlanDesktop() result).
  virtual DesktopSpec spec() const = 0;
  // Changes the size and scale in place. Nothing may be capturing the
  // display meanwhile. On failure the desktop stays as it was if possible;
  // spec() says what it is.
  virtual bool Resize(const DesktopSpec& planned, std::string* error) = 0;
  // Where it is on the desktop: its top-left corner in points (the main
  // desktop is at 0, 0). False when not known.
  virtual bool Origin(int32_t* x, int32_t* y) const {
    (void)x, (void)y;
    return false;
  }
};

// Builds a desktop for a PlanDesktop() result; null with *error if it cannot.
using VirtualDesktopFactory =
    std::function<std::unique_ptr<VirtualDesktop>(const DesktopSpec& planned, std::string* error)>;

// The system's factory (CGVirtualDisplay).
VirtualDesktopFactory SystemVirtualDesktop();

// One more display of an extended desktop (docs/protocol.md, "扩展桌面"):
// built after the main one, next to it with its top-left corner at (x, y)
// points (the system may move it to touch the others; Origin() says where
// it ended up), not mirrored. `number` (2, 3, 4) tells the displays apart.
using ExtraDesktopFactory = std::function<std::unique_ptr<VirtualDesktop>(
    const DesktopSpec& planned, int32_t x, int32_t y, int number, std::string* error)>;
ExtraDesktopFactory SystemExtraDesktop();

// While it exists, the displays do not sleep for being idle (macOS: a
// PreventUserIdleDisplaySleep power assertion), as during Apple's Screen
// Sharing: a display that goes to sleep stops the screen capture, and the
// client's picture would stand still until the remote user clicks or types.
// Creating it also wakes a display that is asleep already.
class DisplayAwake {
 public:
  virtual ~DisplayAwake() = default;
};
std::unique_ptr<DisplayAwake> KeepDisplaysAwake(const std::string& reason);

}  // namespace rdm
