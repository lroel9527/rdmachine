#pragma once
// Where rdmachine keeps its files, and the server's TLS identity in it.
#include <optional>
#include <string>

#include "rdmachine/tls.h"

namespace rdm {

// $RDMACHINE_CONFIG_DIR if set; otherwise ~/Library/Application Support/rdmachine
// on macOS and $XDG_CONFIG_HOME/rdmachine (~/.config/rdmachine) elsewhere.
std::string DefaultConfigDir();

// Creates `dir` and its parents (new directories get mode 0700).
bool MakeDirs(const std::string& dir, std::string* error);

// Reads a whole file; nullopt if it does not exist or cannot be read.
std::optional<std::string> ReadFile(const std::string& path);

// Replaces `path` atomically (temporary file + rename) with owner-only
// permissions (0600).
bool WritePrivateFile(const std::string& path, const std::string& contents, std::string* error);

// The server's long-lived identity: <dir>/server-cert.pem and
// <dir>/server-key.pem, created on first use (docs/protocol.md, "TLS and
// server identity").
std::optional<TlsIdentity> LoadOrCreateServerIdentity(const std::string& dir, const std::string& common_name,
                                                      std::string* error);

}  // namespace rdm
