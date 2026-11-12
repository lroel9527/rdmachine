#pragma once
// The rdmachine daemon (docs/decisions.md, "守护进程和 agent"): runs as root
// from startup, owns the control port and the server identity, logs users in,
// and hands each session to the agent of the graphical session that owns the
// console (the login window when no one is logged in, else that user's). When
// the console changes hands (someone logs in at the login window, logs out,
// switches users), the session moves to the new owner's agent on the same
// control connection, or ends if the new owner is someone else.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "rdmachine/agent_link.h"
#include "rdmachine/server_session.h"

namespace rdm {

// Who owns the console: uid 0 is the login window (no one logged in).
struct ConsoleOwner {
  uint32_t uid = 0;
  std::string user;
  bool login_window() const { return uid == 0; }
};
// The owner of /dev/console.
ConsoleOwner CurrentConsoleOwner();

// Whether the process on the other end of an agent socket runs code signed
// like this one (macOS; elsewhere: true). An unsigned daemon (a development
// build) accepts any agent and says so in *why.
bool PeerSignedLikeUs(int fd, std::string* why);

struct DaemonOptions {
  std::string server_name;
  std::string socket_path = DefaultAgentSocketPath();
  std::string pam_service;  // empty = DefaultPamService()
  std::function<void(const std::string&)> log;
  // For tests: who owns the console, password checks, which agents to trust.
  std::function<ConsoleOwner()> console;
  std::function<bool(const std::string& user, const std::string& password, std::string* error)> check_password;
  std::function<bool(int fd, std::string* why)> trust_agent;
  // A login window agent must run as root and a user's agent as that user.
  // Tests run every agent as themselves.
  bool check_agent_uid = true;
  // What agents may ask of the control server (set by whoever runs it):
  std::function<std::string()> issue_pairing_code;          // empty if pairing is off
  std::function<int64_t()> pairing_code_remaining_ms;
  std::function<void()> close_connections;                  // the menu's Disconnect
  // The console user's agent sent its settings (port, pairing codes).
  std::function<void(uint16_t port, bool offer_pairing_codes)> settings_changed;
  std::function<std::string()> fingerprint;                 // for the agents' menus
  std::function<uint16_t()> port;
  int reply_timeout_ms = 20000;  // an agent starting a session (building a desktop) takes a while
};

class Daemon : public ServerDelegate {
 public:
  explicit Daemon(DaemonOptions options);
  ~Daemon() override;  // Stop()

  // Listens for agents. False with *error if the socket cannot be made.
  bool Start(std::string* error);
  void Stop();

  std::string ServerName() override;
  bool CheckPassword(const std::string& username, const std::string& password, std::string* error) override;
  std::optional<SessionGrant> StartSession(const SessionRequest& request, const SessionSecrets& secrets,
                                           const SessionOrigin& origin, std::string* error) override;
  void EndSession() override;
  SessionCheck CheckSession(const SessionOrigin& origin) override;
  std::string PairingAccount() override;

  // For status and tests.
  int agent_count();
  bool has_active_agent();

 private:
  struct Agent {
    int64_t id = 0;
    std::string kind;  // "aqua" or "loginwindow"
    uint32_t uid = 0;
    std::string user;
    std::unique_ptr<AgentChannel> channel;
    std::thread reader;
    std::atomic<bool> gone{false};
    std::atomic<bool> leaving{false};  // said it is quitting (not a crash)
    std::chrono::steady_clock::time_point gone_at{};  // guarded by mu_
  };
  void AcceptLoop();
  void ReadLoop(std::shared_ptr<Agent> agent);
  void Handle(const std::shared_ptr<Agent>& agent, const nlohmann::json& m);
  // The agent of the console's owner (newest first); null if none.
  std::shared_ptr<Agent> ActiveAgentLocked(const ConsoleOwner& console);
  ConsoleOwner Console();
  // Sends a request and waits for the reply with the same "id".
  std::optional<nlohmann::json> Request(const std::shared_ptr<Agent>& agent, nlohmann::json m, int timeout_ms);
  void Log(const std::string& line);

  const DaemonOptions options_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread accept_thread_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::map<int64_t, std::shared_ptr<Agent>> agents_;
  int64_t next_agent_id_ = 1;
  int64_t next_request_id_ = 1;
  std::map<int64_t, nlohmann::json> replies_;  // request id -> reply
  std::shared_ptr<Agent> session_agent_;       // where the running session is
  // The session's server was stopped on purpose (see CheckSession).
  struct Stopped {
    bool active = false;
    bool login_window = false;
    uint32_t uid = 0;
    std::chrono::steady_clock::time_point at{};
  };
  Stopped stopped_;
  // The password of the last login, for the agent to type into the login
  // window or lock screen (first start after the login only; wiped after).
  struct PendingLogin {
    std::string username, password;
    std::chrono::steady_clock::time_point at{};
  };
  PendingLogin pending_login_;
};

}  // namespace rdm
