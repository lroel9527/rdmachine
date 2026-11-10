#include "rdmachine/session/live_stats.h"

#include <algorithm>

namespace rdm {

namespace {

double Pct(uint64_t lost, uint64_t sent) { return sent ? 100.0 * static_cast<double>(lost) / sent : 0.0; }

// Counters restart when rdaccel starts over (a new transport); never go
// negative.
uint64_t Delta(uint64_t now, uint64_t before) { return now >= before ? now - before : now; }

}  // namespace

LiveStats LiveStatsSampler::Update(const rda_host_stats& s, Clock::time_point now) {
  LiveStats out;
  out.target_kbps = static_cast<double>(s.target_bitrate_bps) / 1000.0;
  out.bandwidth_kbps = static_cast<double>(s.bandwidth_estimate_bps) / 1000.0;
  out.rtt_ms = static_cast<double>(s.rtt_us) / 1000.0;
  out.frames_dropped_congestion = s.frames_dropped_congestion;
  out.keyframes = s.keyframes;
  const bool measured = s.packets_sent > 0;
  // Resends of packets that were only late (jitter) are not loss.
  auto real_lost = [](const rda_host_stats& h) { return h.packets_lost - std::min(h.packets_lost, h.packets_spurious); };
  if (measured) out.total_loss_pct = Pct(real_lost(s), s.packets_sent);

  if (have_prev_) {
    const double dt = std::chrono::duration<double>(now - prev_time_).count();
    if (dt > 0) {
      out.valid = true;
      out.encoded_fps = static_cast<double>(Delta(s.frames_encoded, prev_.frames_encoded)) / dt;
      out.captured_fps = static_cast<double>(Delta(s.frames_captured, prev_.frames_captured)) / dt;
      out.bitrate_kbps = static_cast<double>(Delta(s.bytes_sent, prev_.bytes_sent)) * 8.0 / 1000.0 / dt;
    }
    // rdaccel samples the transport only when a frame goes out, so on a
    // static screen the packet counters stand still: keep the last figure.
    const uint64_t sent = Delta(s.packets_sent, prev_.packets_sent);
    if (sent > 0) last_recent_loss_pct_ = Pct(Delta(real_lost(s), real_lost(prev_)), sent);
  }
  if (!measured) last_recent_loss_pct_ = -1;
  out.recent_loss_pct = last_recent_loss_pct_;

  prev_ = s;
  prev_time_ = now;
  have_prev_ = true;
  return out;
}

}  // namespace rdm
