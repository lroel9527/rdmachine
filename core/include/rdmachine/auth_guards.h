#pragma once
// Server-side guards around login (docs/protocol.md, "auth"): failure rate
// limiting per source address, the pairing code shown on the server, and the
// one-time tokens that admit a viewer to an rdaccel session. All thread-safe;
// time is passed in so tests can control it.
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rdmachine/protocol.h"

namespace rdm {

using Clock = std::chrono::steady_clock;

// After `max_failures` failed logins from one source within `window`, every
// login from it is refused for `block` (counted from the last failure).
class FailureLimiter {
 public:
  FailureLimiter(int max_failures = 5, Clock::duration window = std::chrono::minutes(1),
                 Clock::duration block = std::chrono::minutes(1));

  // How long the source must still wait, or nullopt if it may try now.
  std::optional<std::chrono::milliseconds> RetryAfter(const std::string& source, Clock::time_point now);
  void RecordFailure(const std::string& source, Clock::time_point now);
  void RecordSuccess(const std::string& source);

 private:
  struct Entry {
    std::vector<Clock::time_point> failures;  // within the window
    std::optional<Clock::time_point> blocked_until;
  };
  void Prune(Entry& e, Clock::time_point now);

  const int max_failures_;
  const Clock::duration window_, block_;
  std::mutex mu_;
  std::map<std::string, Entry> entries_;
};

// One pairing code at a time: 8 digits, valid for `lifetime`, used once,
// void after `max_wrong` wrong attempts.
class PairingCodes {
 public:
  enum class Result { kOk, kWrong, kExpired, kNone };

  explicit PairingCodes(Clock::duration lifetime = std::chrono::minutes(5), int max_wrong = 3);

  // Issues a new code (replacing any previous one); empty on RNG failure.
  std::string Issue(Clock::time_point now);
  void Revoke();
  Result Check(const std::string& code, Clock::time_point now);
  bool active(Clock::time_point now);

 private:
  const Clock::duration lifetime_;
  const int max_wrong_;
  std::mutex mu_;
  std::string code_;
  Clock::time_point expires_{};
  int wrong_ = 0;
};

// Tokens handed to a client in session_ready and checked by the rdaccel host's
// on_auth: each admits one viewer, once, within `lifetime`.
class OneTimeTokens {
 public:
  using Token = std::array<uint8_t, kAuthTokenBytes>;

  explicit OneTimeTokens(Clock::duration lifetime = std::chrono::seconds(30));

  void Add(const Token& token, Clock::time_point now);
  // Constant-time comparison; a matching token is removed.
  bool Consume(std::span<const uint8_t> presented, Clock::time_point now);
  size_t size();

 private:
  const Clock::duration lifetime_;
  std::mutex mu_;
  std::vector<std::pair<Token, Clock::time_point>> tokens_;  // token, expiry
};

}  // namespace rdm
