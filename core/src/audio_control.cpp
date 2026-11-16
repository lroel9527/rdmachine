#include "rdmachine/audio_control.h"

#include <nlohmann/json.hpp>

namespace rdm {

std::string EncodeAudioControl(const AudioControl& m) { return nlohmann::json{{"paused", m.paused}}.dump(); }

std::optional<AudioControl> DecodeAudioControl(const std::string& payload) {
  const nlohmann::json j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  AudioControl m;
  if (auto it = j.find("paused"); it != j.end() && it->is_boolean()) m.paused = it->get<bool>();
  return m;
}

}  // namespace rdm
