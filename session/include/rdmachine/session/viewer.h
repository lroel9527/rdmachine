#pragma once
// Client side: turn a session_ready into a connected rdaccel viewer.
#include <string>

#include <rdaccel/rdaccel.h>

#include "rdmachine/messages.h"

namespace rdm {

// Creates the viewer for `ready` against `host` (the same host the control
// connection went to). `config` carries the application's callbacks; the
// key, token and transport are filled in here. The viewer then connects in
// the background: watch on_state for RDA_STATE_CONNECTED.
rda_status CreateSessionViewer(const std::string& host, const SessionReady& ready, rda_viewer_config config,
                               rda_viewer** out, std::string* error);

}  // namespace rdm
