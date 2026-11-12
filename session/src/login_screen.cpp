#include "rdmachine/session/login_screen.h"

namespace rdm {

// Linux: the server shares an X display that has no lock screen of ours to
// get past yet.
LoginScreenState QueryLoginScreen() { return {}; }

}  // namespace rdm
