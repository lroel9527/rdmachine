#pragma once
// The client's pinned server fingerprints, like SSH's known_hosts: one line
// per server, "<host>:<port> sha256:<hex>". Lines starting with # are kept.
#include <cstdint>
#include <optional>
#include <string>

namespace rdm {

class KnownHosts {
 public:
  explicit KnownHosts(std::string path) : path_(std::move(path)) {}

  static std::string Key(const std::string& host, uint16_t port);

  std::optional<std::string> Lookup(const std::string& key) const;
  // Adds or replaces the entry.
  bool Remember(const std::string& key, const std::string& fingerprint, std::string* error);
  bool Forget(const std::string& key, std::string* error);

  enum class Verdict { kTrusted, kUnknown, kChanged };
  Verdict Check(const std::string& key, const std::string& fingerprint) const;

  const std::string& path() const { return path_; }

 private:
  bool Rewrite(const std::string& key, const std::string* fingerprint, std::string* error);
  std::string path_;
};

}  // namespace rdm
