#include "rdmachine/ime_wire.h"

#include <pwd.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace rdm {

namespace {

using nlohmann::json;

bool GetRange(const json& j, const char* key, ImeRange* out) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number_unsigned() ||
      !(*it)[1].is_number_unsigned()) {
    return false;
  }
  out->location = (*it)[0].get<uint32_t>();
  out->length = (*it)[1].get<uint32_t>();
  return true;
}

}  // namespace

std::string EncodeImeInput(const ImeInput& m) {
  json j{{"seq", m.seq}};
  switch (m.kind) {
    case ImeInput::Kind::kMark:
      j["mark"] = m.text;
      j["sel"] = {m.sel_location, m.sel_length};
      break;
    case ImeInput::Kind::kCommit: j["commit"] = m.text; break;
    case ImeInput::Kind::kCancel: j["cancel"] = true; break;
    case ImeInput::Kind::kMode: j["mode"] = m.mode_on ? "on" : "off"; break;
  }
  return j.dump();
}

std::optional<ImeInput> DecodeImeInput(const std::string& payload) {
  const json j = json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  auto seq = j.find("seq");
  if (seq == j.end() || !seq->is_number_unsigned()) return std::nullopt;
  ImeInput m;
  m.seq = seq->get<uint64_t>();
  int kinds = 0;
  if (auto it = j.find("mark"); it != j.end()) {
    if (!it->is_string()) return std::nullopt;
    m.kind = ImeInput::Kind::kMark;
    m.text = it->get<std::string>();
    ImeRange sel;
    if (j.contains("sel")) {
      if (!GetRange(j, "sel", &sel)) return std::nullopt;
      m.sel_location = sel.location;
      m.sel_length = sel.length;
    }
    ++kinds;
  }
  if (auto it = j.find("commit"); it != j.end()) {
    if (!it->is_string()) return std::nullopt;
    m.kind = ImeInput::Kind::kCommit;
    m.text = it->get<std::string>();
    ++kinds;
  }
  if (auto it = j.find("cancel"); it != j.end()) {
    if (!it->is_boolean() || !it->get<bool>()) return std::nullopt;
    m.kind = ImeInput::Kind::kCancel;
    ++kinds;
  }
  if (auto it = j.find("mode"); it != j.end()) {
    if (!it->is_string() || (*it != "on" && *it != "off")) return std::nullopt;
    m.kind = ImeInput::Kind::kMode;
    m.mode_on = *it == "on";
    ++kinds;
  }
  if (kinds != 1) return std::nullopt;
  return m;
}

std::string EncodeImeState(const ImeState& s) {
  json j{{"available", s.available}, {"client", s.client}, {"secure", s.secure}, {"seq", s.seq}};
  if (!s.app.empty()) j["app"] = s.app;
  if (s.caret) {
    j["caret"] = {{"display", s.caret->display}, {"x", s.caret->x}, {"y", s.caret->y},
                  {"w", s.caret->width},       {"h", s.caret->height}};
  }
  if (s.marked) j["marked"] = {s.marked->location, s.marked->length};
  if (s.selected) j["sel"] = {s.selected->location, s.selected->length};
  if (s.ended) j["ended"] = true;
  return j.dump();
}

std::optional<ImeState> DecodeImeState(const std::string& payload) {
  const json j = json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  ImeState s;
  auto flag = [&](const char* key, bool* out) {
    if (auto it = j.find(key); it != j.end() && it->is_boolean()) *out = it->get<bool>();
  };
  flag("available", &s.available);
  flag("client", &s.client);
  flag("secure", &s.secure);
  flag("ended", &s.ended);
  if (auto it = j.find("app"); it != j.end() && it->is_string()) s.app = it->get<std::string>();
  if (auto it = j.find("seq"); it != j.end() && it->is_number_unsigned()) s.seq = it->get<uint64_t>();
  if (auto it = j.find("caret"); it != j.end() && it->is_object()) {
    ImeRect r;
    bool ok = true;
    auto num = [&](const char* key, int32_t* out) {
      auto f = it->find(key);
      if (f == it->end() || !f->is_number_integer()) {
        ok = false;
        return;
      }
      *out = f->get<int32_t>();
    };
    num("display", &r.display);
    num("x", &r.x);
    num("y", &r.y);
    num("w", &r.width);
    num("h", &r.height);
    if (ok) s.caret = r;
  }
  ImeRange range;
  if (GetRange(j, "marked", &range)) s.marked = range;
  if (GetRange(j, "sel", &range)) s.selected = range;
  return s;
}

std::string ImeSocketPath() {
  const passwd* pw = getpwuid(getuid());
  if (!pw || !pw->pw_dir || !*pw->pw_dir) return {};
  return std::string(pw->pw_dir) + "/Library/Application Support/RDMachine/ime.sock";
}

}  // namespace rdm
