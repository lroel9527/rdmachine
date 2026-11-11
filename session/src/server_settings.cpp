#include "rdmachine/session/server_settings.h"

#include <nlohmann/json.hpp>

#include "rdmachine/session/config_store.h"

namespace rdm {

std::string ServerSettingsPath(const std::string& config_dir) { return config_dir + "/server-settings.json"; }

bool LoadServerSettings(const std::string& config_dir, ServerSettings* out, std::string* error) {
  *out = ServerSettings{};
  auto text = ReadFile(ServerSettingsPath(config_dir));
  if (!text) return true;
  nlohmann::json j = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) {
    *error = ServerSettingsPath(config_dir) + " is not a JSON object";
    return false;
  }
  auto num = [&](const char* key, int64_t lo, int64_t hi, auto* field) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) {
      const int64_t v = it->get<int64_t>();
      if (v >= lo && v <= hi) *field = static_cast<std::remove_pointer_t<decltype(field)>>(v);
    }
  };
  auto flag = [&](const char* key, bool* field) {
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) *field = it->get<bool>();
  };
  num("port", 1, 65534, &out->port);  // the TCP fallback needs port + 1
  flag("offer_pairing_codes", &out->offer_pairing_codes);
  flag("view_only", &out->view_only);
  if (auto it = j.find("clipboard"); it != j.end() && it->is_string()) {
    if (auto m = ParseClipboardMode(it->get<std::string>())) out->clipboard_mode = *m;
  }
  num("clipboard_file_limit_mb", 0, 1 << 20, &out->clipboard_file_limit_mb);
  num("fps", 0, 240, &out->fps);
  num("bitrate_kbps", 0, 1'000'000, &out->bitrate_kbps);
  return true;
}

bool SaveServerSettings(const std::string& config_dir, const ServerSettings& s, std::string* error) {
  if (!MakeDirs(config_dir, error)) return false;
  nlohmann::json j = {
      {"port", s.port},
      {"offer_pairing_codes", s.offer_pairing_codes},
      {"view_only", s.view_only},
      {"clipboard", ClipboardModeName(s.clipboard_mode)},
      {"clipboard_file_limit_mb", s.clipboard_file_limit_mb},
      {"fps", s.fps},
      {"bitrate_kbps", s.bitrate_kbps},
  };
  return WritePrivateFile(ServerSettingsPath(config_dir), j.dump(2) + "\n", error);
}

}  // namespace rdm
