#include "rdmachine/input_source_sync.h"

#include <nlohmann/json.hpp>

namespace rdm {

std::string EncodeInputSource(const InputSourceState& s) {
  return nlohmann::json{{"id", s.id},
                        {"name", s.name},
                        {"language", s.language},
                        {"input_method", s.input_method},
                        {"ascii_capable", s.ascii_capable}}
      .dump();
}

std::optional<InputSourceState> DecodeInputSource(const std::string& payload) {
  const nlohmann::json j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  InputSourceState s;
  auto text = [&](const char* key, std::string* out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) *out = it->get<std::string>();
  };
  auto flag = [&](const char* key, bool* out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) *out = it->get<bool>();
  };
  text("id", &s.id);
  text("name", &s.name);
  text("language", &s.language);
  flag("input_method", &s.input_method);
  flag("ascii_capable", &s.ascii_capable);
  if (s.id.empty() || s.id.size() > 255) return std::nullopt;
  return s;
}

namespace {

std::string Primary(const std::string& lang) { return lang.substr(0, lang.find('-')); }

}  // namespace

std::optional<std::string> ChooseInputSource(const InputSourceState& wanted,
                                             const std::vector<InputSourceState>& available, bool layouts_only) {
  if (layouts_only && wanted.input_method) {
    for (const char* id : {"com.apple.keylayout.ABC", "com.apple.keylayout.US"}) {
      for (const auto& a : available) {
        if (a.id == id && !a.input_method) return a.id;
      }
    }
    for (const auto& a : available) {
      if (!a.input_method && a.ascii_capable) return a.id;
    }
    return std::nullopt;
  }
  for (const auto& a : available) {
    if (a.id == wanted.id && !(layouts_only && a.input_method)) return a.id;
  }
  // Candidates of the same kind; the best language match wins.
  auto pick = [&](auto kind_ok) -> std::optional<std::string> {
    const InputSourceState* same_lang = nullptr;
    const InputSourceState* same_primary = nullptr;
    const InputSourceState* any = nullptr;
    for (const auto& a : available) {
      if (!kind_ok(a)) continue;
      if (!wanted.language.empty() && a.language == wanted.language && !same_lang) same_lang = &a;
      if (!wanted.language.empty() && Primary(a.language) == Primary(wanted.language) && !same_primary)
        same_primary = &a;
      if (!any) any = &a;
    }
    if (same_lang) return same_lang->id;
    if (same_primary) return same_primary->id;
    if (any && !wanted.input_method) return any->id;  // any ASCII layout will do for plain typing
    return std::nullopt;
  };
  if (wanted.input_method) {
    // A language's input method only; Pinyin is no stand-in for Kana.
    auto r = pick([](const InputSourceState& a) { return a.input_method; });
    if (r) return r;
    return std::nullopt;
  }
  if (wanted.ascii_capable) return pick([](const InputSourceState& a) { return !a.input_method && a.ascii_capable; });
  // A non-ASCII layout (Russian, Greek): the same language's layout or nothing.
  const InputSourceState* same = nullptr;
  for (const auto& a : available) {
    if (!a.input_method && !wanted.language.empty() && Primary(a.language) == Primary(wanted.language)) {
      same = &a;
      break;
    }
  }
  if (same) return same->id;
  return std::nullopt;
}

}  // namespace rdm
