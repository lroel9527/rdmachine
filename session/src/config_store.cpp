#include "rdmachine/session/config_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace rdm {

std::string DefaultConfigDir() {
  if (const char* d = std::getenv("RDMACHINE_CONFIG_DIR"); d && *d) return d;
  const char* home = std::getenv("HOME");
  const std::string h = home ? home : ".";
#if defined(__APPLE__)
  return h + "/Library/Application Support/rdmachine";
#else
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/rdmachine";
  return h + "/.config/rdmachine";
#endif
}

bool MakeDirs(const std::string& dir, std::string* error) {
  if (dir.empty()) return true;
  struct stat st {};
  if (stat(dir.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) return true;
    if (error) *error = dir + " exists and is not a directory";
    return false;
  }
  const auto slash = dir.find_last_of('/');
  if (slash != std::string::npos && slash > 0 && !MakeDirs(dir.substr(0, slash), error)) return false;
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    if (error) *error = "cannot create " + dir + ": " + std::strerror(errno);
    return false;
  }
  return true;
}

std::optional<std::string> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) return std::nullopt;
  return ss.str();
}

bool WritePrivateFile(const std::string& path, const std::string& contents, std::string* error) {
  const std::string tmp = path + ".tmp" + std::to_string(getpid());
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (error) *error = "cannot write " + tmp + ": " + std::strerror(errno);
    return false;
  }
  fchmod(fd, 0600);  // in case the file already existed with other bits
  size_t done = 0;
  while (done < contents.size()) {
    const ssize_t n = write(fd, contents.data() + done, contents.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (error) *error = "cannot write " + tmp + ": " + std::strerror(errno);
      close(fd);
      unlink(tmp.c_str());
      return false;
    }
    done += static_cast<size_t>(n);
  }
  if (fsync(fd) != 0 || close(fd) != 0) {
    if (error) *error = "cannot write " + tmp + ": " + std::strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    if (error) *error = "cannot replace " + path + ": " + std::strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::optional<TlsIdentity> LoadOrCreateServerIdentity(const std::string& dir, const std::string& common_name,
                                                      std::string* error) {
  const std::string cert_path = dir + "/server-cert.pem";
  const std::string key_path = dir + "/server-key.pem";
  auto cert = ReadFile(cert_path);
  auto key = ReadFile(key_path);
  if (cert && key) {
    if (!CertificateFingerprint(*cert)) {
      if (error) *error = cert_path + " is not a valid certificate";
      return std::nullopt;
    }
    return TlsIdentity{*cert, *key};
  }
  if (cert || key) {
    if (error) *error = "only one of " + cert_path + " and " + key_path + " exists; remove it to make a new identity";
    return std::nullopt;
  }
  if (!MakeDirs(dir, error)) return std::nullopt;
  auto id = GenerateTlsIdentity(common_name, error);
  if (!id) return std::nullopt;
  // Key first: a crash in between leaves a key without certificate, which
  // the check above reports instead of silently making a new identity.
  if (!WritePrivateFile(key_path, id->key_pem, error) || !WritePrivateFile(cert_path, id->cert_pem, error))
    return std::nullopt;
  return id;
}

}  // namespace rdm
