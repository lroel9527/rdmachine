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
};

// Builds a desktop for a PlanDesktop() result; null with *error if it cannot.
using VirtualDesktopFactory =
    std::function<std::unique_ptr<VirtualDesktop>(const DesktopSpec& planned, std::string* error)>;

// This platform's factory; empty where there is none (Linux for now).
VirtualDesktopFactory SystemVirtualDesktop();

}  // namespace rdm
