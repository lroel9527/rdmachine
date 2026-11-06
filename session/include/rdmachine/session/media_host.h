#pragma once
// The server's ServerDelegate: checks passwords with the system and runs one
// rdaccel host per session (capture, encode, input injection), admitting only
// the viewer that presents the session's one-time token.
#include <functional>
#include <mutex>
#include <string>

#include <rdaccel/rdaccel.h>

#include "rdmachine/auth_guards.h"
#include "rdmachine/server_session.h"

namespace rdm {

struct MediaHostOptions {
  std::string server_name;
  // rdaccel listens here: UDP on the control port (a different protocol, so
  // no clash), TCP fallback on the next port (docs/protocol.md).
  uint16_t udp_port = kDefaultPort;
  uint16_t tcp_port = kDefaultPort + 1;
  // Password check. Default: PAM (`pam_service`, empty = DefaultPamService())
  // and, in M0, only the account the server runs as (not_console_user).
  std::function<bool(const std::string& user, const std::string& password, std::string* error)> check_password;
  std::string pam_service;
  bool inject_input = true;  // false: every session is view-only
  int32_t fps = 0;           // 0 = rdaccel's default
  int64_t bitrate_bps = 0;   // 0 = rdaccel's default
  std::function<void(const std::string&)> log;  // may be empty
};

class MediaHost : public ServerDelegate {
 public:
  explicit MediaHost(MediaHostOptions options);
  ~MediaHost() override;  // ends a running session

  std::string ServerName() override;
  bool CheckPassword(const std::string& username, const std::string& password, std::string* error) override;
  std::optional<SessionGrant> StartSession(const SessionRequest& request, const SessionSecrets& secrets,
                                           std::string* error) override;
  void EndSession() override;

  // Whether an rdaccel host is running (for tests and status).
  bool session_running();

 private:
  static int OnAuth(void* user, const uint8_t* token, size_t n);
  static void OnInput(void* user, const rda_input_event* ev);
  static void OnState(void* user, rda_state state, rda_status reason);
  static void OnError(void* user, rda_status status, const char* message);
  void Log(const std::string& line);

  const MediaHostOptions options_;
  OneTimeTokens tokens_;
  std::mutex mu_;  // guards host_ and injector_ changes
  rda_host* host_ = nullptr;
  rda_injector* injector_ = nullptr;  // null for view-only sessions
};

}  // namespace rdm
