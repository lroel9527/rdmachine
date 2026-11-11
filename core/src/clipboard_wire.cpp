#include "rdmachine/clipboard_wire.h"

#include <nlohmann/json.hpp>

namespace rdm {
namespace {

using nlohmann::json;

constexpr size_t kMaxFormats = 16;
constexpr size_t kMaxMimeBytes = 100;

json Parse(std::string_view payload) {
  return json::parse(payload.begin(), payload.end(), nullptr, /*allow_exceptions=*/false);
}

template <typename T>
bool Get(const json& j, const char* key, T* out) {
  auto it = j.find(key);
  if (it == j.end()) return false;
  if constexpr (std::is_same_v<T, std::string>) {
    if (!it->is_string()) return false;
  } else if constexpr (std::is_same_v<T, bool>) {
    if (!it->is_boolean()) return false;
  } else if constexpr (std::is_unsigned_v<T>) {
    if (!it->is_number_unsigned()) return false;
  } else {
    if (!it->is_number_integer()) return false;
  }
  *out = it->get<T>();
  return true;
}

bool GoodMime(const std::string& m) { return !m.empty() && m.size() <= kMaxMimeBytes; }

void PutLe(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

uint64_t GetLe(std::string_view s, size_t at) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
  return v;
}

}  // namespace

std::string EncodeClipboardHello(const ClipboardHello& h) { return json{{"version", h.version}}.dump(); }

std::optional<ClipboardHello> DecodeClipboardHello(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  ClipboardHello h;
  h.version = 0;
  if (!Get(j, "version", &h.version) || h.version < 1) return std::nullopt;
  return h;
}

std::string EncodeClipboardAnnounce(const ClipboardAnnounce& a) {
  return json{{"id", a.id}, {"formats", a.formats}}.dump();
}

std::optional<ClipboardAnnounce> DecodeClipboardAnnounce(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  ClipboardAnnounce a;
  if (!Get(j, "id", &a.id)) return std::nullopt;
  if (auto it = j.find("formats"); it != j.end()) {
    if (!it->is_array() || it->size() > kMaxFormats) return std::nullopt;
    for (const auto& f : *it) {
      if (!f.is_string() || !GoodMime(f.get<std::string>())) return std::nullopt;
      a.formats.push_back(f.get<std::string>());
    }
  }
  return a;
}

std::string EncodeClipboardRequest(const ClipboardRequest& r) {
  return json{{"announce", r.announce}, {"id", r.id}, {"format", r.format}}.dump();
}

std::optional<ClipboardRequest> DecodeClipboardRequest(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  ClipboardRequest r;
  if (!Get(j, "announce", &r.announce) || !Get(j, "id", &r.id) || !Get(j, "format", &r.format) ||
      !GoodMime(r.format)) {
    return std::nullopt;
  }
  return r;
}

std::string EncodeClipboardOffer(const ClipboardOffer& o) {
  json j{{"id", o.id}, {"request", o.request}};
  if (o.skipped.empty()) {
    j["format"] = o.format;
    j["size"] = o.size;
  } else {
    j["skipped"] = o.skipped;
  }
  return j.dump();
}

std::optional<ClipboardOffer> DecodeClipboardOffer(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  ClipboardOffer o;
  if (!Get(j, "id", &o.id) || !Get(j, "request", &o.request)) return std::nullopt;
  if (Get(j, "skipped", &o.skipped) && !o.skipped.empty()) return o;
  if (!Get(j, "format", &o.format) || !GoodMime(o.format) || !Get(j, "size", &o.size)) return std::nullopt;
  return o;
}

std::string EncodeClipboardData(const ClipboardDataHeader& h, std::string_view bytes) {
  std::string s;
  s.reserve(kClipboardDataHeaderSize + bytes.size());
  PutLe(s, h.id);
  PutLe(s, h.offset);
  s.append(bytes);
  return s;
}

std::optional<ClipboardDataHeader> DecodeClipboardData(std::string_view payload, std::string_view* bytes) {
  if (payload.size() < kClipboardDataHeaderSize) return std::nullopt;
  ClipboardDataHeader h;
  h.id = GetLe(payload, 0);
  h.offset = GetLe(payload, 8);
  *bytes = payload.substr(kClipboardDataHeaderSize);
  return h;
}

std::string EncodeClipboardAck(const ClipboardAck& a) {
  std::string s;
  PutLe(s, a.id);
  PutLe(s, a.bytes);
  return s;
}

std::optional<ClipboardAck> DecodeClipboardAck(std::string_view payload) {
  if (payload.size() != 16) return std::nullopt;
  return ClipboardAck{GetLe(payload, 0), GetLe(payload, 8)};
}

std::string EncodeClipboardCancel(const ClipboardCancel& c) {
  return json{{"id", c.id}, {"from_sender", c.from_sender}, {"reason", c.reason}}.dump();
}

std::optional<ClipboardCancel> DecodeClipboardCancel(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  ClipboardCancel c;
  if (!Get(j, "id", &c.id) || !Get(j, "from_sender", &c.from_sender)) return std::nullopt;
  Get(j, "reason", &c.reason);
  return c;
}

}  // namespace rdm
