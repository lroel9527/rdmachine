#include "rdmachine/audio_control.h"

#include <nlohmann/json.hpp>

namespace rdm {

std::string AudioOffLine(bool session_has_audio, const std::string& audio_reason, int revision, bool requested,
                         bool wanted_now) {
  if (session_has_audio) return {};
  if (revision < 3) return "Audio: off (older server)";
  if (audio_reason == "disabled") return "Audio: off (server setting)";
  if (audio_reason == "login_window") return "Audio: off (login window)";
  if (audio_reason == "tcp") return "Audio: off (over TCP there is no sound)";
  if (audio_reason == "unsupported") return "Audio: off (server cannot capture)";
  if (!wanted_now) return "Audio: off (turned off here)";
  // Started with sound off here, turned on since: the next connection has it.
  if (!requested) return "Audio: off (reconnect to hear sound)";
  return "Audio: off";
}

std::string EncodeAudioControl(const AudioControl& m) { return nlohmann::json{{"paused", m.paused}}.dump(); }

std::optional<AudioControl> DecodeAudioControl(const std::string& payload) {
  const nlohmann::json j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  AudioControl m;
  if (auto it = j.find("paused"); it != j.end() && it->is_boolean()) m.paused = it->get<bool>();
  return m;
}

}  // namespace rdm
