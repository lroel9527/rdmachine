#include "rdmachine/auth_guards.h"

#include <algorithm>

#include "rdmachine/random.h"

namespace rdm {

// ---------------------------------------------------------------------------
// FailureLimiter

FailureLimiter::FailureLimiter(int max_failures, Clock::duration window, Clock::duration block)
    : max_failures_(max_failures), window_(window), block_(block) {}

void FailureLimiter::Prune(Entry& e, Clock::time_point now) {
  std::erase_if(e.failures, [&](Clock::time_point t) { return now - t >= window_; });
  if (e.blocked_until && now >= *e.blocked_until) e.blocked_until.reset();
}

std::optional<std::chrono::milliseconds> FailureLimiter::RetryAfter(const std::string& source,
                                                                    Clock::time_point now) {
  std::lock_guard lock(mu_);
  auto it = entries_.find(source);
  if (it == entries_.end()) return std::nullopt;
  Prune(it->second, now);
  if (!it->second.blocked_until) {
    if (it->second.failures.empty()) entries_.erase(it);
    return std::nullopt;
  }
  auto left = std::chrono::ceil<std::chrono::milliseconds>(*it->second.blocked_until - now);
  return std::max(left, std::chrono::milliseconds(1));
}

void FailureLimiter::RecordFailure(const std::string& source, Clock::time_point now) {
  std::lock_guard lock(mu_);
  Entry& e = entries_[source];
  Prune(e, now);
  e.failures.push_back(now);
  if (static_cast<int>(e.failures.size()) >= max_failures_) e.blocked_until = now + block_;
}

void FailureLimiter::RecordSuccess(const std::string& source) {
  std::lock_guard lock(mu_);
  entries_.erase(source);
}

// ---------------------------------------------------------------------------
// PairingCodes

OneTimeTokens::OneTimeTokens(Clock::duration lifetime) : lifetime_(lifetime) {}

void OneTimeTokens::Add(const Token& token, Clock::time_point now) {
  std::lock_guard lock(mu_);
  std::erase_if(tokens_, [&](const auto& t) { return now >= t.second; });
  tokens_.emplace_back(token, now + lifetime_);
}

bool OneTimeTokens::Consume(std::span<const uint8_t> presented, Clock::time_point now) {
  std::lock_guard lock(mu_);
  std::erase_if(tokens_, [&](const auto& t) { return now >= t.second; });
  if (presented.size() != kAuthTokenBytes) return false;
  for (auto it = tokens_.begin(); it != tokens_.end(); ++it) {
    uint8_t diff = 0;
    for (size_t i = 0; i < kAuthTokenBytes; ++i) diff |= static_cast<uint8_t>(it->first[i] ^ presented[i]);
    if (diff == 0) {
      tokens_.erase(it);
      return true;
    }
  }
  return false;
}

size_t OneTimeTokens::size() {
  std::lock_guard lock(mu_);
  return tokens_.size();
}

}  // namespace rdm
