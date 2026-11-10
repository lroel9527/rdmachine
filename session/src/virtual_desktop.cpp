#include "rdmachine/session/virtual_desktop.h"

namespace rdm {

// No virtual display on this platform yet: the server shares its own display
// and the client scales the picture.
VirtualDesktopFactory SystemVirtualDesktop() { return {}; }

}  // namespace rdm
