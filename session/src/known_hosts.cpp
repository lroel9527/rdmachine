#include "rdmachine/session/known_hosts.h"

#include <sstream>

#include "rdmachine/session/config_store.h"

namespace rdm {

namespace {

// Splits "key fingerprint" lines; returns false for blank lines and comments.
bool ParseLine(const std::string& line, std::string* key, std::string* fp) {
  std::istringstream in(line);
  if (!(in >> *key) || (*key)[0] == '#') return false;
  return static_cast<bool>(in >> *fp);
}

}  // namespace

std::string KnownHosts::Key(const std::string& host, uint16_t port) {
  // Bracket IPv6 literals so the port stays unambiguous.
  const bool v6 = host.find(':') != std::string::npos && host.front() != '[';
  return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

std::optional<std::string> KnownHosts::Lookup(const std::string& key) const {
  auto text = ReadFile(path_);
  if (!text) return std::nullopt;
  std::istringstream in(*text);
  std::string line, k, fp;
  while (std::getline(in, line)) {
    if (ParseLine(line, &k, &fp) && k == key) return fp;
  }
  return std::nullopt;
}

KnownHosts::Verdict KnownHosts::Check(const std::string& key, const std::string& fingerprint) const {
  auto pinned = Lookup(key);
  if (!pinned) return Verdict::kUnknown;
  return *pinned == fingerprint ? Verdict::kTrusted : Verdict::kChanged;
}

bool KnownHosts::Rewrite(const std::string& key, const std::string* fingerprint, std::string* error) {
  std::string out;
  if (auto text = ReadFile(path_)) {
    std::istringstream in(*text);
    std::string line, k, fp;
    while (std::getline(in, line)) {
      if (ParseLine(line, &k, &fp) && k == key) continue;
      out += line + "\n";
    }
  }
  if (fingerprint) out += key + " " + *fingerprint + "\n";
  const auto slash = path_.find_last_of('/');
  if (slash != std::string::npos && !MakeDirs(path_.substr(0, slash), error)) return false;
  return WritePrivateFile(path_, out, error);
}

bool KnownHosts::Remember(const std::string& key, const std::string& fingerprint, std::string* error) {
  return Rewrite(key, &fingerprint, error);
}

bool KnownHosts::Forget(const std::string& key, std::string* error) { return Rewrite(key, nullptr, error); }

}  // namespace rdm
