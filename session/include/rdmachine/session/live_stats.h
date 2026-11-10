#pragma once
// Turns rdaccel's cumulative host counters into the rates a status display
// shows (frames per second, bitrate, recent packet loss). Pure arithmetic on
// samples, so it is tested without a screen or network.
#include <chrono>
#include <cstdint>

#include <rdaccel/rdaccel.h>

namespace rdm {

struct LiveStats {
  bool valid = false;      // false until two samples have been seen
  double encoded_fps = 0;  // frames sent to the viewer
  double captured_fps = 0; // frames the screen delivered
  double bitrate_kbps = 0; // everything the host sent
  double target_kbps = 0;  // encoder budget (adaptive bitrate); 0 = fixed
  double bandwidth_kbps = 0;  // transport's estimate; 0 = not measured (TCP)
  double rtt_ms = 0;          // 0 = not measured (TCP)
  // Lost / sent packets since the previous sample with traffic, and over the
  // whole session; negative = not measured (TCP). Real loss: resends of
  // packets that were only late (rda_host_stats.packets_spurious) are left out.
  double recent_loss_pct = -1;
  double total_loss_pct = -1;
  uint64_t frames_dropped_congestion = 0;  // whole session
  uint64_t keyframes = 0;                  // whole session
};

class LiveStatsSampler {
 public:
  using Clock = std::chrono::steady_clock;

  // Feeds the next sample and returns the rates since the previous one.
  LiveStats Update(const rda_host_stats& s, Clock::time_point now);
  // A new session: counters start over.
  void Reset() { have_prev_ = false; }

 private:
  bool have_prev_ = false;
  rda_host_stats prev_{};
  Clock::time_point prev_time_{};
  double last_recent_loss_pct_ = -1;
};

}  // namespace rdm
