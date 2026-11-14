#pragma once
// Where files copied on the peer appear when pasted here (core's
// FileTarget; docs/file-paste.md).
#include <memory>
#include <string>

#include "rdmachine/file_transfer.h"

namespace rdm {

// macOS: the File Provider extension inside this app (a domain called
// "RDMachine"): the peer's items become placeholders that Finder copies,
// fetched when it does. One extension and domain per process, shared by the
// sessions of the process (each call gives a handle; the last session that
// published owns what is on offer). Null when this process has no extension
// (a command-line tool, the login window, Linux).
std::shared_ptr<FileTarget> SystemFileTarget();

// Adds this app's File Provider domain and waits for the answer: the
// switch in System Settings cannot be turned on until the domain exists.
// The installer runs it (`--register-files`, as the logged-in user) so the
// switch is there right after installing; otherwise the first paste adds
// the domain. False with `error` when this process has no extension or the
// system refused.
bool RegisterFileDomain(std::string* error);

// Where fetched items are written before the extension takes them:
// ~/Library/Caches/<bundle id>/files (macOS).
std::string FileStagingDir();

}  // namespace rdm
