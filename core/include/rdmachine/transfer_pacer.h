#pragma once
// How fast clipboard data may go out (docs/protocol.md, "剪贴板").
//
// rdaccel sends application messages ahead of the picture, so data handed
// to it all at once would hold every frame back until it is through, and
// its rate control would see the queue and lower the picture's bitrate.
// The sender therefore paces itself, as background traffic: it starts
// slowly and speeds up while nothing suffers, and slows down when
//  - the receiver's acknowledgements take noticeably longer than the
//    shortest seen (a queue is building somewhere on the way, or the
//    receiver's disk cannot keep up), or
//  - the picture had to skip frames for congestion (the server knows;
//    OnCongestion).
// Platform independent and clock-free (times are passed in), for tests.
#include <cstdint>

namespace rdm {

class TransferPacer {
 public:
  struct Options {
    double start_bps = 8e6;
    double min_bps = 256e3;
    double max_bps = 4e9;
    // Acknowledgements this much slower than the base count as a queue.
    uint64_t queue_target_us = 25'000;
    uint64_t epoch_us = 100'000;  // how often the rate is reconsidered
    double increase = 1.25;       // per epoch without trouble
    double decrease = 0.7;        // per epoch with trouble
  };

  TransferPacer();
  explicit TransferPacer(Options options);

  // Whether `bytes` more may be sent now, with `in_flight` bytes sent and
  // not yet acknowledged.
  bool CanSend(uint64_t now_us, uint64_t bytes, uint64_t in_flight);
  // When CanSend may next say yes for `bytes` (now_us if it does already);
  // a full window waits for acknowledgements, not for time.
  uint64_t NextSendUs(uint64_t now_us, uint64_t bytes) const;
  void OnSent(uint64_t now_us, uint64_t bytes);
  // An acknowledgement came back `delay_us` after the data it covers left.
  void OnAck(uint64_t now_us, uint64_t delay_us);
  // The picture had to skip frames since the last call.
  void OnCongestion(uint64_t now_us);

  double rate_bps() const { return rate_bps_; }
  // Most bytes that may be unacknowledged at the current rate.
  uint64_t Window() const;
  uint64_t base_delay_us() const { return base_delay_us_; }

 private:
  void Refill(uint64_t now_us);
  void MaybeEndEpoch(uint64_t now_us);

  Options o_;
  double rate_bps_;
  double tokens_ = 0;  // bytes
  uint64_t tokens_us_ = 0;
  bool started_ = false;

  // The shortest acknowledgement delay of the last minute or so, kept as
  // the minimum of two half-windows.
  uint64_t base_delay_us_ = 0;
  uint64_t base_window_[2] = {UINT64_MAX, UINT64_MAX};
  uint64_t base_window_start_us_ = 0;

  uint64_t epoch_start_us_ = 0;
  uint64_t epoch_bytes_ = 0;
  uint64_t epoch_min_delay_us_ = UINT64_MAX;
  bool epoch_trouble_ = false;
};

}  // namespace rdm
