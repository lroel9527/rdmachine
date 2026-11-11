#include "rdmachine/transfer_pacer.h"

#include <algorithm>
#include <cmath>

namespace rdm {
namespace {

constexpr uint64_t kBaseHalfWindowUs = 30'000'000;
// What the window allows beyond one base round trip: room for the data a
// round trip at the current rate needs, plus some for jitter.
constexpr uint64_t kWindowSlackUs = 200'000;
constexpr uint64_t kMinWindow = 256 * 1024;
// The bucket holds at most this much sending time, so a pause does not let
// a burst out afterwards.
constexpr uint64_t kBurstUs = 20'000;
constexpr uint64_t kMinBurst = 64 * 1024;

}  // namespace

TransferPacer::TransferPacer() : TransferPacer(Options{}) {}

TransferPacer::TransferPacer(Options options) : o_(options), rate_bps_(options.start_bps) {}

uint64_t TransferPacer::Window() const {
  const double rtt_s = static_cast<double>(base_delay_us_ + kWindowSlackUs) / 1e6;
  return std::max<uint64_t>(kMinWindow, static_cast<uint64_t>(rate_bps_ / 8 * rtt_s));
}

void TransferPacer::Refill(uint64_t now_us) {
  if (!started_) {
    started_ = true;
    tokens_us_ = now_us;
    epoch_start_us_ = now_us;
    base_window_start_us_ = now_us;
    tokens_ = static_cast<double>(kMinBurst);
    return;
  }
  if (now_us <= tokens_us_) return;
  const double cap = std::max<double>(kMinBurst, rate_bps_ / 8 * static_cast<double>(kBurstUs) / 1e6);
  tokens_ = std::min(cap, tokens_ + rate_bps_ / 8 * static_cast<double>(now_us - tokens_us_) / 1e6);
  tokens_us_ = now_us;
}

bool TransferPacer::CanSend(uint64_t now_us, uint64_t bytes, uint64_t in_flight) {
  Refill(now_us);
  MaybeEndEpoch(now_us);
  if (in_flight > 0 && in_flight + bytes > Window()) return false;
  // A chunk larger than the bucket goes once the bucket is full.
  const double cap = std::max<double>(kMinBurst, rate_bps_ / 8 * static_cast<double>(kBurstUs) / 1e6);
  return tokens_ >= std::min(static_cast<double>(bytes), cap);
}

uint64_t TransferPacer::NextSendUs(uint64_t now_us, uint64_t bytes) const {
  const double cap = std::max<double>(kMinBurst, rate_bps_ / 8 * static_cast<double>(kBurstUs) / 1e6);
  const double need = std::min(static_cast<double>(bytes), cap) - tokens_;
  if (need <= 0) return now_us;
  return now_us + static_cast<uint64_t>(std::ceil(need * 8 / rate_bps_ * 1e6));
}

void TransferPacer::OnSent(uint64_t now_us, uint64_t bytes) {
  Refill(now_us);
  tokens_ -= static_cast<double>(bytes);
  epoch_bytes_ += bytes;
}

void TransferPacer::OnAck(uint64_t now_us, uint64_t delay_us) {
  Refill(now_us);
  if (now_us - base_window_start_us_ > kBaseHalfWindowUs) {
    base_window_[0] = base_window_[1];
    base_window_[1] = UINT64_MAX;
    base_window_start_us_ = now_us;
  }
  base_window_[1] = std::min(base_window_[1], delay_us);
  base_delay_us_ = std::min(base_window_[0], base_window_[1]);
  epoch_min_delay_us_ = std::min(epoch_min_delay_us_, delay_us);
  MaybeEndEpoch(now_us);
}

void TransferPacer::OnCongestion(uint64_t now_us) {
  Refill(now_us);
  epoch_trouble_ = true;
  MaybeEndEpoch(now_us);
}

void TransferPacer::MaybeEndEpoch(uint64_t now_us) {
  if (now_us - epoch_start_us_ < o_.epoch_us) return;
  const uint64_t elapsed = now_us - epoch_start_us_;
  // A queue: even the quickest acknowledgement of the epoch was slow.
  const bool queue = epoch_min_delay_us_ != UINT64_MAX && epoch_min_delay_us_ > base_delay_us_ + o_.queue_target_us;
  // Only a sender that used most of what the rate allowed learns anything
  // about the path; one held back by its window or its disk does not.
  const double allowed = rate_bps_ / 8 * static_cast<double>(elapsed) / 1e6;
  const bool used = static_cast<double>(epoch_bytes_) >= 0.5 * allowed;
  if (epoch_trouble_ || queue) {
    rate_bps_ = std::max(o_.min_bps, rate_bps_ * o_.decrease);
  } else if (used) {
    rate_bps_ = std::min(o_.max_bps, rate_bps_ * o_.increase);
  }
  epoch_start_us_ = now_us;
  epoch_bytes_ = 0;
  epoch_min_delay_us_ = UINT64_MAX;
  epoch_trouble_ = false;
}

}  // namespace rdm
