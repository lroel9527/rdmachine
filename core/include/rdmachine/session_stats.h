#pragma once
// Session statistics for the client's overlay (docs/protocol.md, "会话统计"):
// the server measures (rdaccel's host stats) and sends a report about once a
// second; the client shows the fields its user picked. Platform independent.
#include <cstdint>
#include <optional>
#include <string>

namespace rdm {

// rdaccel application message type (RDA_MSG_USER_FIRST + 2), server to client.
constexpr uint32_t kSessionStatsMessageType = 0x10002;

struct SessionStatsReport {
  int32_t width = 0, height = 0;
  double encoded_fps = 0, captured_fps = 0;
  double bitrate_kbps = 0, target_kbps = 0;  // target 0 = fixed bitrate
  double bandwidth_kbps = 0;                 // 0 = not measured yet
  double rtt_ms = 0;                         // 0 = not measured yet
  double recent_loss_pct = -1, total_loss_pct = -1;  // negative = not measured yet
  uint64_t frames_dropped_congestion = 0;
  std::string encoder;
  std::string input_source;  // what the server selected for the client's; may be empty
  bool view_only = false;
  bool text_mode = false;  // the picture is sent in text mode (picture_mode.h)
  // Sound (revision 3): "off", "starting", "on", "silent", "paused",
  // "no_permission" or "failed"; empty from an older server. audio_kbps:
  // the last second's sound bitrate, counted as bitrate_kbps is (it is also
  // in bitrate_kbps).
  std::string audio;
  double audio_kbps = 0;

  bool operator==(const SessionStatsReport&) const = default;
};

std::string EncodeSessionStats(const SessionStatsReport& r);
// nullopt for anything that is not a JSON object; missing fields keep defaults.
std::optional<SessionStatsReport> DecodeSessionStats(const std::string& payload);

}  // namespace rdm
