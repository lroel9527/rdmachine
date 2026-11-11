#include "rdmachine/clipboard_wire.h"

#include <set>

#include <nlohmann/json.hpp>

#include "rdmachine/clipboard_sync.h"  // IsValidUtf8

namespace rdm {
namespace {

using nlohmann::json;

// Generous upper bounds for what a peer may describe: the sender's own
// limits are far lower, these only keep a broken peer from making this side
// allocate without end.
constexpr size_t kMaxEntries = 100000;
constexpr size_t kMaxFormats = 16;
constexpr size_t kMaxPathBytes = 4096;
constexpr uint64_t kMaxItemBytes = uint64_t{1} << 46;  // 64 TiB

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

void PutLe(std::string& s, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

uint64_t GetLe(std::string_view s, size_t at, int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
  return v;
}

const char* KindName(OfferEntry::Kind k) {
  switch (k) {
    case OfferEntry::Kind::kFile: return "file";
    case OfferEntry::Kind::kDir: return "dir";
    case OfferEntry::Kind::kLink: return "link";
  }
  return "file";
}

bool Fail(std::string* why, const std::string& text) {
  if (why) *why = text;
  return false;
}

}  // namespace

std::string EncodeClipboardHello(const ClipboardHello& h) {
  return json{{"version", h.version}, {"max_file_bytes", h.max_file_bytes}}.dump();
}

std::optional<ClipboardHello> DecodeClipboardHello(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  ClipboardHello h;
  h.version = 0;
  if (!Get(j, "version", &h.version) || h.version < 1) return std::nullopt;
  Get(j, "max_file_bytes", &h.max_file_bytes);  // missing: no files
  return h;
}

std::vector<uint64_t> ClipboardOffer::ItemSizes() const {
  std::vector<uint64_t> sizes;
  sizes.reserve(formats.size() + entries.size());
  for (const auto& f : formats) sizes.push_back(f.size);
  for (const auto& e : entries) {
    if (e.kind == OfferEntry::Kind::kFile) sizes.push_back(e.size);
  }
  return sizes;
}

uint64_t ClipboardOffer::TotalBytes() const {
  uint64_t n = 0;
  for (uint64_t s : ItemSizes()) n += s;
  return n;
}

uint64_t ClipboardOffer::FileBytes() const {
  uint64_t n = 0;
  for (const auto& e : entries) {
    if (e.kind == OfferEntry::Kind::kFile) n += e.size;
  }
  return n;
}

size_t ClipboardOffer::FileCount() const {
  size_t n = 0;
  for (const auto& e : entries) n += e.kind == OfferEntry::Kind::kFile;
  return n;
}

std::string EncodeClipboardOffer(const ClipboardOffer& o) {
  json formats = json::array();
  for (const auto& f : o.formats) formats.push_back({{"mime", f.mime}, {"size", f.size}});
  json entries = json::array();
  for (const auto& e : o.entries) {
    json je{{"path", e.path}, {"kind", KindName(e.kind)}};
    if (e.kind == OfferEntry::Kind::kFile) je["size"] = e.size;
    if (e.kind == OfferEntry::Kind::kLink) je["target"] = e.target;
    if (e.mode != 0) je["mode"] = e.mode;
    if (e.mtime != 0) je["mtime"] = e.mtime;
    entries.push_back(std::move(je));
  }
  json j{{"id", o.id}, {"formats", std::move(formats)}, {"entries", std::move(entries)}};
  if (o.skipped_files > 0 || !o.skipped_reason.empty()) {
    j["skipped"] = {{"files", o.skipped_files},
                    {"bytes", o.skipped_bytes},
                    {"reason", o.skipped_reason},
                    {"limit", o.skipped_limit}};
  }
  return j.dump();
}

std::optional<ClipboardOffer> DecodeClipboardOffer(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  ClipboardOffer o;
  if (!Get(j, "id", &o.id)) return std::nullopt;
  if (auto it = j.find("formats"); it != j.end()) {
    if (!it->is_array() || it->size() > kMaxFormats) return std::nullopt;
    for (const auto& jf : *it) {
      if (!jf.is_object()) return std::nullopt;
      OfferFormat f;
      if (!Get(jf, "mime", &f.mime) || !Get(jf, "size", &f.size)) return std::nullopt;
      o.formats.push_back(std::move(f));
    }
  }
  if (auto it = j.find("entries"); it != j.end()) {
    if (!it->is_array() || it->size() > kMaxEntries) return std::nullopt;
    for (const auto& je : *it) {
      if (!je.is_object()) return std::nullopt;
      OfferEntry e;
      std::string kind;
      if (!Get(je, "path", &e.path) || !Get(je, "kind", &kind)) return std::nullopt;
      if (kind == "file") {
        e.kind = OfferEntry::Kind::kFile;
        if (!Get(je, "size", &e.size)) return std::nullopt;
      } else if (kind == "dir") {
        e.kind = OfferEntry::Kind::kDir;
      } else if (kind == "link") {
        e.kind = OfferEntry::Kind::kLink;
        if (!Get(je, "target", &e.target)) return std::nullopt;
      } else {
        return std::nullopt;
      }
      Get(je, "mode", &e.mode);
      Get(je, "mtime", &e.mtime);
      o.entries.push_back(std::move(e));
    }
  }
  if (auto it = j.find("skipped"); it != j.end() && it->is_object()) {
    Get(*it, "files", &o.skipped_files);
    Get(*it, "bytes", &o.skipped_bytes);
    Get(*it, "reason", &o.skipped_reason);
    Get(*it, "limit", &o.skipped_limit);
  }
  if (!ValidateOffer(o)) return std::nullopt;
  return o;
}

bool IsSafeRelativePath(std::string_view path) {
  if (path.empty() || path.size() > kMaxPathBytes || !IsValidUtf8(path)) return false;
  size_t start = 0;
  while (true) {
    const size_t slash = path.find('/', start);
    const std::string_view name = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (name.empty() || name.size() > 255 || name == "." || name == "..") return false;
    for (char c : name) {
      const auto u = static_cast<unsigned char>(c);
      if (u < 0x20 || u == 0x7F) return false;
    }
    if (slash == std::string_view::npos) return true;
    start = slash + 1;
  }
}

bool ValidateOffer(const ClipboardOffer& o, std::string* why) {
  if (o.formats.size() > kMaxFormats) return Fail(why, "too many formats");
  if (o.entries.size() > kMaxEntries) return Fail(why, "too many entries");
  std::set<std::string_view> mimes;
  for (const auto& f : o.formats) {
    if (f.mime.empty() || f.mime.size() > 100 || !mimes.insert(f.mime).second) return Fail(why, "bad format name");
    if (f.size > kMaxItemBytes) return Fail(why, "format too large");
  }
  std::set<std::string_view> dirs, seen;
  for (const auto& e : o.entries) {
    if (!IsSafeRelativePath(e.path)) return Fail(why, "unsafe path");
    if (!seen.insert(e.path).second) return Fail(why, "path listed twice");
    const size_t slash = e.path.rfind('/');
    if (slash != std::string::npos && !dirs.count(std::string_view(e.path).substr(0, slash))) {
      return Fail(why, "parent not listed before: " + e.path);
    }
    if (e.kind == OfferEntry::Kind::kDir) dirs.insert(e.path);
    if (e.size > kMaxItemBytes) return Fail(why, "file too large");
    if (e.mode > 0777) return Fail(why, "bad mode");
    if (e.kind == OfferEntry::Kind::kLink &&
        (e.target.empty() || e.target.size() > kMaxPathBytes || e.target.find('\0') != std::string::npos)) {
      return Fail(why, "bad link target");
    }
  }
  return true;
}

std::string EncodeClipboardData(const ClipboardDataHeader& h, std::string_view bytes) {
  std::string s;
  s.reserve(kClipboardDataHeaderSize + bytes.size());
  PutLe(s, h.id, 8);
  PutLe(s, h.item, 4);
  PutLe(s, h.offset, 8);
  s.append(bytes);
  return s;
}

std::optional<ClipboardDataHeader> DecodeClipboardData(std::string_view payload, std::string_view* bytes) {
  if (payload.size() < kClipboardDataHeaderSize) return std::nullopt;
  ClipboardDataHeader h;
  h.id = GetLe(payload, 0, 8);
  h.item = static_cast<uint32_t>(GetLe(payload, 8, 4));
  h.offset = GetLe(payload, 12, 8);
  *bytes = payload.substr(kClipboardDataHeaderSize);
  return h;
}

std::string EncodeClipboardAck(const ClipboardAck& a) {
  std::string s;
  PutLe(s, a.id, 8);
  PutLe(s, a.bytes, 8);
  return s;
}

std::optional<ClipboardAck> DecodeClipboardAck(std::string_view payload) {
  if (payload.size() != 16) return std::nullopt;
  return ClipboardAck{GetLe(payload, 0, 8), GetLe(payload, 8, 8)};
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
