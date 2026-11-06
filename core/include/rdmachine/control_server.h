#pragma once
// Accepts control connections over TLS and runs a ServerSession on each, on
// its own thread. M0 serves one session at a time (the delegate answers
// `busy` to the rest).
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rdmachine/auth_guards.h"
#include "rdmachine/server_session.h"
#include "rdmachine/tls.h"

namespace rdm {

struct ControlServerOptions {
  std::string bind_address;            // empty = all interfaces
  uint16_t port = kDefaultPort;        // 0 = any free port
  int handshake_timeout_ms = 10000;
  int idle_timeout_ms = 45000;         // no message for this long: close
  int ping_interval_ms = 15000;        // send a ping after this long without traffic
  bool offer_pairing_codes = true;
};

class ControlServer {
 public:
  ControlServer(ServerDelegate& delegate, TlsIdentity identity, ControlServerOptions options = {});
  ~ControlServer();  // Stop()

  bool Start(std::string* error);
  void Stop();
  uint16_t port() const;

  // The code to show on the server's screen; empty if pairing is off.
  std::string IssuePairingCode();

  // Number of connections being served (for tests and status).
  int active_connections() const { return active_.load(); }

 private:
  void AcceptLoop();
  void Serve(std::unique_ptr<TlsConnection> conn);

  ServerDelegate& delegate_;
  const TlsIdentity identity_;
  const ControlServerOptions options_;
  FailureLimiter limiter_;
  PairingCodes pairing_;
  std::unique_ptr<TlsListener> listener_;
  std::atomic<bool> stop_{false};
  std::atomic<int> active_{0};
  std::thread accept_thread_;
  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  void ReapWorkers(bool all);

  std::mutex threads_mu_;
  std::list<Worker> threads_;
  std::mutex conns_mu_;
  std::list<TlsConnection*> conns_;  // open connections, closed by Stop()
};

}  // namespace rdm
