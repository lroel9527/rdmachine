#pragma once
// An agent (docs/decisions.md, "守护进程和 agent"): runs in one graphical
// session (the login window, or a logged-in user's), connects to the daemon
// and runs the sessions the daemon hands it on a MediaHost (capture, encode,
// input). It reconnects by itself when the daemon restarts.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "rdmachine/agent_link.h"
#include "rdmachine/session/media_host.h"

namespace rdm {

struct AgentHostOptions {
  std::string socket_path = DefaultAgentSocketPath();
  std::string kind = "aqua";  // or "loginwindow"
  std::function<void(const std::string&)> log;
  // The menu bar's server settings, sent each time the link comes up (the
  // daemon takes them from the console user's agent only).
  bool send_settings = false;
  uint16_t port = 0;
  bool offer_pairing_codes = true;
};

class AgentHost {
 public:
  // `media` must outlive the agent.
  AgentHost(MediaHost& media, AgentHostOptions options);
  ~AgentHost();  // Stop()

  void Start();  // connects in the background, and again whenever the link drops
  void Stop();   // ends a running session

  bool connected() const { return connected_; }
  // What the daemon said about itself (empty / 0 until connected).
  std::string fingerprint() const;
  uint16_t port() const;
  std::string server_name() const;

  // The menu bar of the console user's session (the daemon ignores these from
  // any other agent). A new pairing code (empty if refused or not connected)
  // and how long it stays valid.
  struct PairingCode {
    std::string code;
    std::chrono::milliseconds remaining{0};
  };
  PairingCode IssuePairingCode();
  std::chrono::milliseconds PairingCodeRemaining();
  void Disconnect();
  void SendSettings(uint16_t port, bool offer_pairing_codes);

 private:
  void Run();
  void Serve(AgentChannel& channel);
  void Handle(AgentChannel& channel, const nlohmann::json& m);
  std::optional<nlohmann::json> Ask(nlohmann::json m, const std::string& reply_type, int timeout_ms);
  void Log(const std::string& line);

  MediaHost& media_;
  const AgentHostOptions options_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  std::thread thread_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  AgentChannel* channel_ = nullptr;  // while connected
  std::string fingerprint_, server_name_;
  uint16_t port_ = 0;
  int64_t next_id_ = 1;
  std::optional<nlohmann::json> reply_;  // the answer Ask() waits for
  std::string reply_type_;
  int64_t reply_id_ = 0;
  bool in_session_ = false;
};

}  // namespace rdm
