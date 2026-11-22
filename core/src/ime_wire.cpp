#include "rdmachine/ime_wire.h"

#include <mbedtls/md.h>
#include <pwd.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "rdmachine/base64.h"

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

json InputToJson(const ImeInput& m) {
  json j{{"seq", m.seq}};
  if (m.ack) j["ack"] = m.ack;
  switch (m.kind) {
    case ImeInput::Kind::kMark:
      j["mark"] = m.text;
      j["sel"] = {m.sel_location, m.sel_length};
      break;
    case ImeInput::Kind::kCommit: j["commit"] = m.text; break;
    case ImeInput::Kind::kCancel: j["cancel"] = true; break;
    case ImeInput::Kind::kMode: j["mode"] = m.mode_on ? "on" : "off"; break;
    case ImeInput::Kind::kKey: {
      json k{{"id", m.key}};
      if (m.im) k["im"] = true;
      if (m.field) k["field"] = *m.field;
      if (!m.events.empty()) {
        json ev = json::array();
        for (const ImeKeyEvent& e : m.events) ev.push_back({e.kind, e.code, e.modifiers});
        k["ev"] = std::move(ev);
      }
      if (!m.then.empty()) {
        json then = json::array();
        for (const ImeInput& t : m.then) then.push_back(InputToJson(t));
        k["then"] = std::move(then);
      }
      j["key"] = std::move(k);
      break;
    }
    case ImeInput::Kind::kSource: {
      json s{{"id", m.source_id}, {"icon", m.icon}};
      if (!m.png.empty()) s["png"] = Base64Encode(m.png);
      j["source"] = std::move(s);
      break;
    }
  }
  return j;
}

// nested: inside a key's "then", where only marks, commits and cancels go.
std::optional<ImeInput> InputFromJson(const json& j, bool nested) {
  if (!j.is_object()) return std::nullopt;
  auto seq = j.find("seq");
  if (seq == j.end() || !seq->is_number_unsigned()) return std::nullopt;
  ImeInput m;
  m.seq = seq->get<uint64_t>();
  if (auto it = j.find("ack"); it != j.end()) {
    if (!it->is_number_unsigned()) return std::nullopt;
    m.ack = it->get<uint64_t>();
  }
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
    if (nested || !it->is_string() || (*it != "on" && *it != "off")) return std::nullopt;
    m.kind = ImeInput::Kind::kMode;
    m.mode_on = *it == "on";
    ++kinds;
  }
  if (auto it = j.find("key"); it != j.end()) {
    if (nested || !it->is_object()) return std::nullopt;
    const json& k = *it;
    auto id = k.find("id");
    if (id == k.end() || !id->is_number_unsigned()) return std::nullopt;
    m.kind = ImeInput::Kind::kKey;
    m.key = id->get<uint64_t>();
    if (auto f = k.find("im"); f != k.end()) {
      if (!f->is_boolean()) return std::nullopt;
      m.im = f->get<bool>();
    }
    if (auto f = k.find("field"); f != k.end()) {
      if (!f->is_boolean()) return std::nullopt;
      m.field = f->get<bool>();
    }
    if (auto f = k.find("ev"); f != k.end()) {
      if (!f->is_array()) return std::nullopt;
      for (const json& e : *f) {
        if (!e.is_array() || e.size() != 3) return std::nullopt;
        for (const json& n : e) {
          if (!n.is_number_unsigned() || n.get<uint64_t>() > UINT32_MAX) return std::nullopt;
        }
        m.events.push_back({e[0].get<uint32_t>(), e[1].get<uint32_t>(), e[2].get<uint32_t>()});
      }
    }
    if (auto f = k.find("then"); f != k.end()) {
      if (!f->is_array()) return std::nullopt;
      for (const json& t : *f) {
        auto in = InputFromJson(t, /*nested=*/true);
        if (!in) return std::nullopt;
        m.then.push_back(std::move(*in));
      }
    }
    ++kinds;
  }
  if (auto it = j.find("source"); it != j.end()) {
    if (nested || !it->is_object()) return std::nullopt;
    const json& s = *it;
    auto id = s.find("id");
    auto icon = s.find("icon");
    if (id == s.end() || !id->is_string() || icon == s.end() || !icon->is_string()) return std::nullopt;
    m.kind = ImeInput::Kind::kSource;
    m.source_id = id->get<std::string>();
    m.icon = icon->get<std::string>();
    if (!m.icon.empty() && !IsImeIconFingerprint(m.icon)) return std::nullopt;
    if (auto p = s.find("png"); p != s.end()) {
      if (!p->is_string() || m.icon.empty()) return std::nullopt;
      auto bytes = Base64Decode(p->get<std::string>());
      if (!bytes || bytes->empty() || bytes->size() > kImeIconMaxBytes) return std::nullopt;
      m.png = std::move(*bytes);
    }
    ++kinds;
  }
  if (kinds != 1) return std::nullopt;
  return m;
}

}  // namespace

std::string EncodeImeInput(const ImeInput& m) { return InputToJson(m).dump(); }

std::optional<ImeInput> DecodeImeInput(const std::string& payload) {
  return InputFromJson(json::parse(payload, nullptr, /*allow_exceptions=*/false), /*nested=*/false);
}

std::string ImeIconFingerprint(const std::vector<uint8_t>& png) {
  uint8_t digest[32];
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), png.data(), png.size(), digest);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < 16; ++i) {
    out += kHex[digest[i] >> 4];
    out += kHex[digest[i] & 15];
  }
  return out;
}

bool IsImeIconFingerprint(const std::string& s) {
  if (s.size() != 32) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::string EncodeImeState(const ImeState& s) {
  json j{{"available", s.available}, {"client", s.client}, {"secure", s.secure}, {"seq", s.seq}};
  if (!s.app.empty()) j["app"] = s.app;
  if (s.field) j["field"] = s.field;
  if (s.caret) {
    j["caret"] = {{"display", s.caret->display}, {"x", s.caret->x}, {"y", s.caret->y},
                  {"w", s.caret->width},       {"h", s.caret->height}};
  }
  if (s.marked) j["marked"] = {s.marked->location, s.marked->length};
  if (s.selected) j["sel"] = {s.selected->location, s.selected->length};
  if (s.ended) j["ended"] = true;
  if (s.keys) j["keys"] = true;
  if (s.back) j["back"] = s.back;
  if (s.typed) j["typed"] = s.typed;
  if (s.judged) j["judged"] = s.judged;
  if (s.badge) j["badge"] = true;
  if (!s.need_icon.empty()) j["need_icon"] = s.need_icon;
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
  flag("keys", &s.keys);
  flag("badge", &s.badge);
  if (auto it = j.find("need_icon"); it != j.end() && it->is_string() && IsImeIconFingerprint(it->get<std::string>())) {
    s.need_icon = it->get<std::string>();
  }
  if (auto it = j.find("app"); it != j.end() && it->is_string()) s.app = it->get<std::string>();
  if (auto it = j.find("seq"); it != j.end() && it->is_number_unsigned()) s.seq = it->get<uint64_t>();
  if (auto it = j.find("back"); it != j.end() && it->is_number_unsigned()) s.back = it->get<uint64_t>();
  if (auto it = j.find("typed"); it != j.end() && it->is_number_unsigned()) s.typed = it->get<uint64_t>();
  if (auto it = j.find("judged"); it != j.end() && it->is_number_unsigned()) s.judged = it->get<uint64_t>();
  if (auto it = j.find("field"); it != j.end() && it->is_number_unsigned() &&
                                 it->get<uint64_t>() <= UINT32_MAX) {
    s.field = static_cast<uint32_t>(it->get<uint64_t>());
  }
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
