#pragma once
// The client's side of the control protocol: connect, check the server's
// fingerprint, log in, request a session, and keep the connection for the
// session's lifetime.
#include <functional>
#include <memory>
#include <string>

#include "rdmachine/channel.h"
#include "rdmachine/messages.h"
#include "rdmachine/tls.h"

namespace rdm {

struct ClientOptions {
  std::string host;
  uint16_t port = kDefaultPort;
  std::string client_name;
  Auth auth;
  SessionRequest request;
  // Called with the server's fingerprint before anything is sent. Return true
  // to continue: the pinned value matches, or the user accepted a first-time
  // fingerprint. Required.
  std::function<bool(const std::string& fingerprint)> trust_server;
  int timeout_ms = 10000;  // per step
};

class ClientConnection {
 public:
  explicit ClientConnection(std::unique_ptr<TlsConnection> conn) : conn_(std::move(conn)), channel_(*conn_) {}
  ~ClientConnection() { Close("user_closed"); }

  MessageChannel& channel() { return channel_; }
  // The server's address this connection reached (the picture connection
  // goes to the same one, not to a fresh lookup of the host name).
  const std::string& peer_ip() const { return conn_->peer_ip(); }
  // Sends bye (once) and closes.
  void Close(const std::string& reason);

 private:
  std::unique_ptr<TlsConnection> conn_;
  MessageChannel channel_;
  bool closed_ = false;
};

struct ConnectResult {
  // Empty on success. Otherwise an error code from docs/protocol.md
  // (bad_credentials, rate_limited, incompatible, busy, ...) or one of the
  // client-side codes: connect_failed, untrusted_server, timeout,
  // connection_closed, bad_message.
  std::string error;
  std::string detail;  // human-readable context for logs
  std::optional<int64_t> retry_after_ms;
  std::string fingerprint;
  ServerHello server;
  SessionReady session;
  std::unique_ptr<ClientConnection> connection;  // set on success
};

ConnectResult ConnectAndStartSession(const ClientOptions& options);

}  // namespace rdm
