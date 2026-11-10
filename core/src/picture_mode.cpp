#include "rdmachine/picture_mode.h"

#include <nlohmann/json.hpp>

namespace rdm {

std::string EncodePictureMode(const PictureMode& m) { return nlohmann::json{{"text_mode", m.text_mode}}.dump(); }

std::optional<PictureMode> DecodePictureMode(const std::string& payload) {
  const nlohmann::json j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  PictureMode m;
  if (auto it = j.find("text_mode"); it != j.end() && it->is_boolean()) m.text_mode = it->get<bool>();
  return m;
}

}  // namespace rdm
