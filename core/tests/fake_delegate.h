#pragma once
#include <atomic>
#include <mutex>

#include "rdmachine/server_session.h"

namespace rdm::testing {

class FakeDelegate : public ServerDelegate {
 public:
  std::string ServerName() override { return "test-mac"; }
  bool CheckPassword(const std::string& user, const std::string& pass, std::string* error) override {
    if (user == "alice" && pass == "secret") return true;
    if (user == "bob") {
      *error = "not_console_user";
      return false;
    }
    *error = "bad_credentials";
    return false;
  }
  std::optional<SessionGrant> StartSession(const SessionRequest& request, const SessionSecrets& secrets,
                                           const SessionOrigin& origin, std::string* error) override {
    std::lock_guard lock(mu);
    if (busy) {
      *error = "busy";
      return std::nullopt;
    }
    if (refuse_starts > 0) {
      --refuse_starts;
      *error = "unavailable";
      return std::nullopt;
    }
    if (request.display_index > 0) {
      *error = "no_display";
      return std::nullopt;
    }
    busy = true;
    ++starts;
    last_request = request;
    last_secrets = secrets;
    last_origin = origin;
    SessionGrant g;
    g.transport = request.transport;
    g.port = 4100;
    g.displays.push_back(DisplayInfo{0, "Built-in", 2880, 1800, 2.0});
    return g;
  }
  void EndSession() override {
    std::lock_guard lock(mu);
    busy = false;
    ++ends;
  }
  SessionCheck CheckSession(const SessionOrigin&) override {
    std::lock_guard lock(mu);
    SessionCheck c = check;
    if (c.action == SessionCheck::Action::kMove) check = {};  // one move per request
    return c;
  }

  std::mutex mu;
  bool busy = false;
  int starts = 0, ends = 0;
  SessionRequest last_request;
  SessionSecrets last_secrets;
  SessionOrigin last_origin;
  SessionCheck check;    // what CheckSession answers next
  int refuse_starts = 0;  // this many StartSession calls fail ("unavailable")
};

}  // namespace rdm::testing
