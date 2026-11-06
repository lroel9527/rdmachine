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

PairingCodes::PairingCodes(Clock::duration lifetime, int max_wrong) : lifetime_(lifetime), max_wrong_(max_wrong) {}

std::string PairingCodes::Issue(Clock::time_point now) {
  // 8 uniformly random digits: rejection-sample bytes below 250 (= 25 * 10).
  std::string code;
  while (code.size() < 8) {
    uint8_t b[16];
    if (!RandomBytes(b)) return {};
    for (uint8_t v : b) {
      if (v < 250 && code.size() < 8) code += static_cast<char>('0' + v % 10);
    }
  }
  std::lock_guard lock(mu_);
  code_ = code;
  expires_ = now + lifetime_;
  wrong_ = 0;
  return code;
}

void PairingCodes::Revoke() {
  std::lock_guard lock(mu_);
  code_.clear();
}

bool PairingCodes::active(Clock::time_point now) {
  std::lock_guard lock(mu_);
  return !code_.empty() && now < expires_;
}

PairingCodes::Result PairingCodes::Check(const std::string& code, Clock::time_point now) {
  std::lock_guard lock(mu_);
  if (code_.empty()) return Result::kNone;
  if (now >= expires_) {
    code_.clear();
    return Result::kExpired;
  }
  // Ignore spaces and dashes people type to group the digits.
  std::string typed;
  for (char c : code) {
    if (c != ' ' && c != '-') typed += c;
  }
  uint8_t diff = typed.size() == code_.size() ? 0 : 1;
  for (size_t i = 0; i < code_.size(); ++i) {
    diff |= static_cast<uint8_t>((i < typed.size() ? typed[i] : 0) ^ code_[i]);
  }
  if (diff == 0) {
    code_.clear();  // used once
    return Result::kOk;
  }
  if (++wrong_ >= max_wrong_) code_.clear();
  return Result::kWrong;
}

// ---------------------------------------------------------------------------
// OneTimeTokens

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
