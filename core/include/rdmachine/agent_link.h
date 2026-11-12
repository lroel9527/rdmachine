#pragma once
// The link between the rdmachine daemon (root, listens on the network,
// logs users in) and its agents (one per graphical session: the login window
// and each logged-in user; they run the picture). docs/protocol.md, "守护进程
// 和 agent", describes the messages.
//
// A local Unix stream socket with the control protocol's framing (4-byte
// length + JSON object with "type"); not encrypted, so it only ever carries
// secrets between processes of this computer, and the daemon only talks to
// agents whose code signature matches its own (macOS) and whose user owns
// the console or is root.
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "rdmachine/frame.h"
#include "rdmachine/messages.h"
#include "rdmachine/server_session.h"

namespace rdm {

inline constexpr int kAgentLinkVersion = 1;

// Where the daemon listens for agents.
std::string DefaultAgentSocketPath();

// Framed JSON messages over a connected Unix socket. One reader at a time;
// Send may be called from any thread.
class AgentChannel {
 public:
  explicit AgentChannel(int fd);  // takes the socket
  ~AgentChannel();
  AgentChannel(const AgentChannel&) = delete;
  AgentChannel& operator=(const AgentChannel&) = delete;

  bool Send(const nlohmann::json& message);
  struct Received {
    enum Status { kMessage, kTimeout, kClosed, kProtocolError } status = kClosed;
    nlohmann::json message;  // an object with a string "type"
    std::string error;
  };
  Received Receive(int timeout_ms);
  // Wakes a blocked Receive (kClosed) and makes Send fail.
  void Shutdown();
  int fd() const { return fd_; }

 private:
  int fd_;
  std::mutex send_mu_;
  FrameDecoder decoder_;
};

// Listens on `path` (a stale socket file there is removed) with permission
// bits `mode`. The listening fd, or -1 with *error.
int UnixListen(const std::string& path, unsigned mode, std::string* error);
// Waits up to timeout_ms for a connection: its fd, or -1 (timeout: *error
// empty).
int UnixAccept(int listen_fd, int timeout_ms, std::string* error);
int UnixConnect(const std::string& path, std::string* error);
// The peer's effective uid and process id.
struct PeerCredentials {
  uint32_t uid = 0;
  int64_t pid = 0;
};
std::optional<PeerCredentials> PeerOf(int fd);

// The pieces of a session the daemon hands to an agent, and back, as JSON.
nlohmann::json ToAgentJson(const SessionRequest& r);
std::optional<SessionRequest> SessionRequestFromAgentJson(const nlohmann::json& j);
nlohmann::json ToAgentJson(const SessionGrant& g);
std::optional<SessionGrant> SessionGrantFromAgentJson(const nlohmann::json& j);
nlohmann::json ToAgentJson(const SessionSecrets& s);
std::optional<SessionSecrets> SessionSecretsFromAgentJson(const nlohmann::json& j);
nlohmann::json ToAgentJson(const SessionOrigin& o);
SessionOrigin SessionOriginFromAgentJson(const nlohmann::json& j);

}  // namespace rdm
