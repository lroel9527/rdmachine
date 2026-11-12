#include "rdmachine/agent_link.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#if defined(__APPLE__)
#include <sys/ucred.h>
#endif

#include "rdmachine/base64.h"

namespace rdm {

using json = nlohmann::json;

std::string DefaultAgentSocketPath() { return "/var/run/rdmachine/agent.sock"; }

namespace {

std::string Errno(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

bool FillAddress(const std::string& path, sockaddr_un* addr, std::string* error) {
  std::memset(addr, 0, sizeof *addr);
  addr->sun_family = AF_UNIX;
  if (path.size() >= sizeof addr->sun_path) {
    *error = "socket path too long: " + path;
    return false;
  }
  std::memcpy(addr->sun_path, path.c_str(), path.size() + 1);
  return true;
}

void NoSigPipe(int fd) {
#if defined(SO_NOSIGPIPE)
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}

}  // namespace

AgentChannel::AgentChannel(int fd) : fd_(fd) { NoSigPipe(fd_); }

AgentChannel::~AgentChannel() {
  if (fd_ >= 0) close(fd_);
}

void AgentChannel::Shutdown() {
  if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
}

bool AgentChannel::Send(const json& message) {
  auto frame = EncodeFrame(message.dump());
  if (!frame) return false;
  std::lock_guard lock(send_mu_);
  size_t off = 0;
  while (off < frame->size()) {
#if defined(MSG_NOSIGNAL)
    const ssize_t n = send(fd_, frame->data() + off, frame->size() - off, MSG_NOSIGNAL);
#else
    const ssize_t n = send(fd_, frame->data() + off, frame->size() - off, 0);
#endif
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

AgentChannel::Received AgentChannel::Receive(int timeout_ms) {
  Received out;
  for (;;) {
    if (auto payload = decoder_.Next()) {
      json j = json::parse(*payload, nullptr, /*allow_exceptions=*/false);
      if (j.is_discarded() || !j.is_object() || !j.contains("type") || !j["type"].is_string()) {
        out.status = Received::kProtocolError;
        out.error = "not a message object";
        return out;
      }
      out.status = Received::kMessage;
      out.message = std::move(j);
      return out;
    }
    pollfd pfd{fd_, POLLIN, 0};
    const int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0 && errno == EINTR) continue;
    if (pr == 0) {
      out.status = Received::kTimeout;
      return out;
    }
    if (pr < 0) {
      out.status = Received::kClosed;
      return out;
    }
    uint8_t buf[16384];
    const ssize_t n = recv(fd_, buf, sizeof buf, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      out.status = Received::kClosed;
      return out;
    }
    if (!decoder_.Feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)))) {
      out.status = Received::kProtocolError;
      out.error = "bad frame";
      return out;
    }
  }
}

int UnixListen(const std::string& path, unsigned mode, std::string* error) {
  sockaddr_un addr;
  if (!FillAddress(path, &addr, error)) return -1;
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = Errno("socket");
    return -1;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  unlink(path.c_str());  // a stale socket from an earlier run
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    *error = Errno(("bind " + path).c_str());
    close(fd);
    return -1;
  }
  chmod(path.c_str(), static_cast<mode_t>(mode));
  if (listen(fd, 8) != 0) {
    *error = Errno("listen");
    close(fd);
    return -1;
  }
  return fd;
}

int UnixAccept(int listen_fd, int timeout_ms, std::string* error) {
  error->clear();
  pollfd pfd{listen_fd, POLLIN, 0};
  const int pr = poll(&pfd, 1, timeout_ms);
  if (pr == 0 || (pr < 0 && errno == EINTR)) return -1;
  if (pr < 0) {
    *error = Errno("poll");
    return -1;
  }
  const int fd = accept(listen_fd, nullptr, nullptr);
  if (fd < 0) {
    if (errno != EINTR && errno != EAGAIN) *error = Errno("accept");
    return -1;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

int UnixConnect(const std::string& path, std::string* error) {
  sockaddr_un addr;
  if (!FillAddress(path, &addr, error)) return -1;
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = Errno("socket");
    return -1;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    *error = Errno(("connect " + path).c_str());
    close(fd);
    return -1;
  }
  return fd;
}

std::optional<PeerCredentials> PeerOf(int fd) {
  PeerCredentials c;
#if defined(__APPLE__)
  uid_t uid = 0;
  gid_t gid = 0;
  if (getpeereid(fd, &uid, &gid) != 0) return std::nullopt;
  c.uid = uid;
  pid_t pid = 0;
  socklen_t len = sizeof pid;
  if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0) c.pid = pid;
#else
  struct ucred cred {};
  socklen_t len = sizeof cred;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return std::nullopt;
  c.uid = cred.uid;
  c.pid = cred.pid;
#endif
  return c;
}

// Sessions: the control protocol's own encoding, so there is one of each.

json ToAgentJson(const SessionRequest& r) { return json::parse(SerializeMessage(Message{r})); }

std::optional<SessionRequest> SessionRequestFromAgentJson(const json& j) {
  if (!j.is_object()) return std::nullopt;
  auto parsed = ParseMessage(j.dump());
  if (!parsed.message) return std::nullopt;
  if (auto* r = std::get_if<SessionRequest>(&*parsed.message)) return *r;
  return std::nullopt;
}

json ToAgentJson(const SessionGrant& g) {
  SessionReady r;
  r.transport = g.transport;
  r.port = g.port;
  r.displays = g.displays;
  r.desktop = g.desktop;
  r.login_window = g.login_window;
  json j = json::parse(SerializeMessage(Message{r}));
  j.erase("key");  // all zero here; the daemon holds the real ones
  j.erase("auth_token");
  return j;
}

std::optional<SessionGrant> SessionGrantFromAgentJson(const json& j) {
  if (!j.is_object()) return std::nullopt;
  json full = j;
  const std::array<uint8_t, kSessionKeyBytes> zero{};
  full["key"] = Base64Encode(zero);
  full["auth_token"] = Base64Encode(std::array<uint8_t, kAuthTokenBytes>{});
  full["type"] = "session_ready";
  auto parsed = ParseMessage(full.dump());
  if (!parsed.message) return std::nullopt;
  auto* r = std::get_if<SessionReady>(&*parsed.message);
  if (!r) return std::nullopt;
  SessionGrant g;
  g.transport = r->transport;
  g.port = r->port;
  g.displays = r->displays;
  g.desktop = r->desktop;
  g.login_window = r->login_window;
  return g;
}

json ToAgentJson(const SessionSecrets& s) {
  return {{"key", Base64Encode(s.key)}, {"auth_token", Base64Encode(s.auth_token)}};
}

std::optional<SessionSecrets> SessionSecretsFromAgentJson(const json& j) {
  if (!j.is_object() || !j.contains("key") || !j.contains("auth_token") || !j["key"].is_string() ||
      !j["auth_token"].is_string())
    return std::nullopt;
  auto key = Base64Decode(j["key"].get<std::string>());
  auto token = Base64Decode(j["auth_token"].get<std::string>());
  if (!key || !token || key->size() != kSessionKeyBytes || token->size() != kAuthTokenBytes) return std::nullopt;
  SessionSecrets s;
  std::copy(key->begin(), key->end(), s.key.begin());
  std::copy(token->begin(), token->end(), s.auth_token.begin());
  std::fill(key->begin(), key->end(), 0);
  return s;
}

json ToAgentJson(const SessionOrigin& o) {
  return {{"peer_ip", o.peer_ip},
          {"method", o.method == AuthMethod::kPairingCode ? "pairing_code" : "password"},
          {"username", o.username},
          {"account", o.account}};
}

SessionOrigin SessionOriginFromAgentJson(const json& j) {
  SessionOrigin o;
  if (!j.is_object()) return o;
  o.peer_ip = j.value("peer_ip", "");
  o.method = j.value("method", "password") == "pairing_code" ? AuthMethod::kPairingCode : AuthMethod::kPassword;
  o.username = j.value("username", "");
  o.account = j.value("account", "");
  return o;
}

}  // namespace rdm
