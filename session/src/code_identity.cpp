#include "rdmachine/session/daemon.h"

namespace rdm {

// Linux: no code signatures to compare; the socket's permissions and the
// peer's uid are what there is.
bool PeerSignedLikeUs(int, std::string* why) {
  why->clear();
  return true;
}

}  // namespace rdm
