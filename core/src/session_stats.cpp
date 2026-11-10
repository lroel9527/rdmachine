#include "rdmachine/session_stats.h"

#include <nlohmann/json.hpp>

namespace rdm {

std::string EncodeSessionStats(const SessionStatsReport& r) {
  return nlohmann::json{{"transport", r.transport},
                        {"width", r.width},
                        {"height", r.height},
                        {"encoded_fps", r.encoded_fps},
                        {"captured_fps", r.captured_fps},
                        {"bitrate_kbps", r.bitrate_kbps},
                        {"target_kbps", r.target_kbps},
                        {"bandwidth_kbps", r.bandwidth_kbps},
                        {"rtt_ms", r.rtt_ms},
                        {"recent_loss_pct", r.recent_loss_pct},
                        {"total_loss_pct", r.total_loss_pct},
                        {"frames_dropped_congestion", r.frames_dropped_congestion},
                        {"encoder", r.encoder},
                        {"input_source", r.input_source},
                        {"view_only", r.view_only},
                        {"text_mode", r.text_mode}}
      .dump();
}

std::optional<SessionStatsReport> DecodeSessionStats(const std::string& payload) {
  const nlohmann::json j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  SessionStatsReport r;
  auto text = [&](const char* k, std::string* out) {
    auto it = j.find(k);
    if (it != j.end() && it->is_string()) *out = it->get<std::string>();
  };
  auto number = [&](const char* k, double* out) {
    auto it = j.find(k);
    if (it != j.end() && it->is_number()) *out = it->get<double>();
  };
  auto integer = [&](const char* k, auto* out) {
    auto it = j.find(k);
    if (it != j.end() && it->is_number_integer()) *out = it->get<std::remove_pointer_t<decltype(out)>>();
  };
  text("transport", &r.transport);
  integer("width", &r.width);
  integer("height", &r.height);
  number("encoded_fps", &r.encoded_fps);
  number("captured_fps", &r.captured_fps);
  number("bitrate_kbps", &r.bitrate_kbps);
  number("target_kbps", &r.target_kbps);
  number("bandwidth_kbps", &r.bandwidth_kbps);
  number("rtt_ms", &r.rtt_ms);
  number("recent_loss_pct", &r.recent_loss_pct);
  number("total_loss_pct", &r.total_loss_pct);
  integer("frames_dropped_congestion", &r.frames_dropped_congestion);
  text("encoder", &r.encoder);
  text("input_source", &r.input_source);
  if (auto it = j.find("view_only"); it != j.end() && it->is_boolean()) r.view_only = it->get<bool>();
  if (auto it = j.find("text_mode"); it != j.end() && it->is_boolean()) r.text_mode = it->get<bool>();
  return r;
}

}  // namespace rdm
