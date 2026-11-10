#include "rdmachine/desktop.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace rdm {

std::optional<DesktopSpec> PlanDesktop(const DesktopSpec& wanted) {
  if (wanted.width <= 0 || wanted.height <= 0 || !std::isfinite(wanted.scale) || wanted.scale <= 0) {
    return std::nullopt;
  }
  DesktopSpec d;
  const int32_t factor = wanted.scale >= 1.5 ? 2 : 1;
  d.scale = factor;
  // A whole number of points, and even pixel sizes at scale 1 as well.
  const int32_t step = 2;
  auto fit = [&](int32_t pixels, int32_t max_pixels) {
    pixels = std::clamp(pixels, kMinDesktopPoints * factor, max_pixels);
    return pixels - pixels % step;
  };
  d.width = fit(wanted.width, kMaxDesktopWidth);
  d.height = fit(wanted.height, kMaxDesktopHeight);
  return d;
}

std::string EncodeDesktopSpec(const DesktopSpec& d) {
  return nlohmann::json{{"width", d.width}, {"height", d.height}, {"scale", d.scale}}.dump();
}

std::optional<DesktopSpec> DecodeDesktopSpec(const std::string& payload) {
  const nlohmann::json j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  auto size = [&](const char* key, int32_t* out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    const int64_t v = it->get<int64_t>();
    if (v <= 0 || v > 1 << 20) return false;
    *out = static_cast<int32_t>(v);
    return true;
  };
  DesktopSpec d;
  if (!size("width", &d.width) || !size("height", &d.height)) return std::nullopt;
  if (auto it = j.find("scale"); it != j.end()) {
    if (!it->is_number()) return std::nullopt;
    d.scale = it->get<double>();
  }
  if (!std::isfinite(d.scale) || d.scale <= 0) return std::nullopt;
  return d;
}

std::string FormatDesktopSpec(const DesktopSpec& d) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%dx%d@%g", d.width, d.height, d.scale);
  return buf;
}

std::optional<DesktopSpec> ParseDesktopSpec(const std::string& text) {
  DesktopSpec d;
  char tail = 0;
  const int n = std::sscanf(text.c_str(), "%dx%d@%lf%c", &d.width, &d.height, &d.scale, &tail);
  if (n != 2 && n != 3) return std::nullopt;
  if (n == 2 && text.find('@') != std::string::npos) return std::nullopt;
  if (d.width <= 0 || d.height <= 0 || !std::isfinite(d.scale) || d.scale <= 0) return std::nullopt;
  return d;
}

}  // namespace rdm
