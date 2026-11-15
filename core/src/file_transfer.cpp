#include "rdmachine/file_transfer.h"

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <nlohmann/json.hpp>

#include "rdmachine/base64.h"
#include "rdmachine/clipboard_sync.h"  // IsValidUtf8
#include "rdmachine/clipboard_wire.h"  // data, ack and cancel encodings
#include "rdmachine/random.h"

namespace rdm {
namespace {

using nlohmann::json;

// Data messages, as the clipboard's.
constexpr size_t kChunk = 128 * 1024;
constexpr uint64_t kCongestionEveryUs = 50'000;
constexpr size_t kMaxHeaderBytes = 64 * 1024;
// Often enough for a progress bar to move smoothly on a slow link.
constexpr uint64_t kProgressEveryBytes = 256 * 1024;

uint64_t SteadyUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

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

const char* KindCode(FileKind k) {
  switch (k) {
    case FileKind::kFile:
      return "f";
    case FileKind::kDir:
      return "d";
    case FileKind::kLink:
      return "l";
  }
  return "f";
}

json EntryJson(const FileEntry& e) {
  json j{{"p", e.path}, {"k", KindCode(e.kind)}, {"m", e.mode}, {"t", e.mtime}};
  if (e.kind == FileKind::kFile) j["s"] = e.size;
  if (e.kind == FileKind::kFile && e.offset > 0) j["o"] = e.offset;
  if (e.kind == FileKind::kLink) j["l"] = e.link;
  if (!e.xattrs.empty()) {
    json x = json::object();
    for (const auto& [name, value] : e.xattrs) {
      x[name] = Base64Encode({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
    }
    j["x"] = std::move(x);
  }
  return j;
}

std::optional<FileEntry> EntryFromJson(const json& j) {
  if (!j.is_object()) return std::nullopt;
  FileEntry e;
  std::string k;
  if (!Get(j, "p", &e.path) || !Get(j, "k", &k)) return std::nullopt;
  if (k == "f") {
    e.kind = FileKind::kFile;
    if (!Get(j, "s", &e.size)) return std::nullopt;
  } else if (k == "d") {
    e.kind = FileKind::kDir;
  } else if (k == "l") {
    e.kind = FileKind::kLink;
    if (!Get(j, "l", &e.link) || e.link.empty() || e.link.size() > kMaxFilePathBytes ||
        e.link.find('\0') != std::string::npos) {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }
  uint32_t mode = 0;
  if (Get(j, "m", &mode)) e.mode = mode & 07777;
  int64_t t = 0;
  if (Get(j, "t", &t)) e.mtime = t;
  if (e.kind == FileKind::kFile && Get(j, "o", &e.offset) && e.offset > e.size) return std::nullopt;
  if (auto x = j.find("x"); x != j.end()) {
    if (!x->is_object()) return std::nullopt;
    size_t total = 0;
    for (auto it = x->begin(); it != x->end(); ++it) {
      if (!it.value().is_string() || it.key().empty() || it.key().size() > 127 ||
          it.key().find('\0') != std::string::npos || !IsValidUtf8(it.key())) {
        return std::nullopt;
      }
      auto v = Base64Decode(it.value().get<std::string>());
      if (!v) return std::nullopt;
      total += it.key().size() + v->size();
      if (total > kMaxXattrBytes) return std::nullopt;
      e.xattrs[it.key()] = std::string(v->begin(), v->end());
    }
  }
  return e;
}

void PutLe32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

uint32_t GetLe32(std::string_view s) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(s[i])) << (8 * i);
  return v;
}

std::string Join(const std::string& dir, const std::string& rel) { return rel.empty() ? dir : dir + "/" + rel; }

bool MakeDirs(const std::string& path) {
  if (path.empty()) return false;
  std::string cur;
  size_t i = 0;
  if (path[0] == '/') {
    cur = "/";
    i = 1;
  }
  while (i <= path.size()) {
    const size_t slash = path.find('/', i);
    const size_t end = slash == std::string::npos ? path.size() : slash;
    if (end > i) {
      if (!cur.empty() && cur.back() != '/') cur += '/';
      cur += path.substr(i, end - i);
      if (mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    if (slash == std::string::npos) break;
    i = slash + 1;
  }
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

int RemoveOne(const char* path, const struct stat*, int, struct FTW*) {
  remove(path);
  return 0;
}

void RemoveTree(const std::string& path) {
  if (path.empty() || path == "/") return;
  nftw(path.c_str(), RemoveOne, 32, FTW_DEPTH | FTW_PHYS);
}

std::string Errno(const std::string& what) { return what + ": " + strerror(errno); }

}  // namespace

bool IsFileMessage(uint32_t type) { return type >= kFileListRequestMessageType && type <= kFileCancelMessageType; }

bool IsSafeRelativePath(std::string_view p, bool allow_empty) {
  if (p.empty()) return allow_empty;
  if (p.size() > kMaxFilePathBytes || p.find('\0') != std::string_view::npos || !IsValidUtf8(p)) return false;
  size_t i = 0;
  while (true) {
    const size_t slash = p.find('/', i);
    const std::string_view part = p.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) return true;
    i = slash + 1;
  }
}

bool IsSafeFileName(std::string_view name) {
  return IsSafeRelativePath(name, false) && name.find('/') == std::string_view::npos && name.size() <= 255;
}

std::string EncodeFileEntries(const std::vector<FileEntry>& entries) {
  json a = json::array();
  for (const auto& e : entries) a.push_back(EntryJson(e));
  return a.dump();
}

std::optional<std::vector<FileEntry>> DecodeFileEntries(std::string_view json_array, bool names_only, size_t max) {
  const json a = Parse(json_array);
  if (!a.is_array() || a.size() > max) return std::nullopt;
  std::vector<FileEntry> out;
  out.reserve(a.size());
  for (const auto& j : a) {
    auto e = EntryFromJson(j);
    if (!e) return std::nullopt;
    if (names_only ? !IsSafeFileName(e->path) : !IsSafeRelativePath(e->path, out.empty())) return std::nullopt;
    if (!names_only && out.empty() && !e->path.empty()) return std::nullopt;  // the item itself first
    out.push_back(std::move(*e));
  }
  return out;
}

std::string EncodeFileListRequest(const FileListRequest& r) {
  return json{{"announce", r.announce}, {"id", r.id}, {"item", r.item}}.dump();
}

std::optional<FileListRequest> DecodeFileListRequest(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  FileListRequest r;
  if (!Get(j, "announce", &r.announce) || !Get(j, "id", &r.id) || !Get(j, "item", &r.item)) return std::nullopt;
  return r;
}

std::string EncodeFileFetch(const FileFetch& f) {
  json j{{"announce", f.announce}, {"id", f.id}, {"item", f.item}};
  if (f.from) {
    json from{{"p", f.from->path}};
    if (f.from->file) {
      from["o"] = f.from->offset;
      from["s"] = f.from->size;
      from["t"] = f.from->mtime;
    }
    j["from"] = std::move(from);
  }
  return j.dump();
}

std::optional<FileFetch> DecodeFileFetch(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  FileFetch f;
  if (!Get(j, "announce", &f.announce) || !Get(j, "id", &f.id) || !Get(j, "item", &f.item)) return std::nullopt;
  if (auto it = j.find("from"); it != j.end() && it->is_object()) {
    FileResumePoint r;
    if (Get(*it, "p", &r.path) && IsSafeRelativePath(r.path, true)) {
      r.file = Get(*it, "o", &r.offset) && Get(*it, "s", &r.size) && Get(*it, "t", &r.mtime) && r.offset <= r.size;
      f.from = std::move(r);
    }
  }
  return f;
}

uint64_t NewCopyId() {
  uint64_t v = 0;
  for (;;) {
    uint8_t b[8];
    if (!RandomBytes(b)) {
      // No system randomness (never seen): the clock, which is unique enough.
      v = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    } else {
      for (uint8_t x : b) v = (v << 8) | x;
    }
    v &= (1ull << 53) - 1;
    if (v != 0) return v;
  }
}

std::string EncodeFileList(const FileList& l) {
  json j{{"request", l.request}};
  if (!l.error.empty()) {
    j["error"] = l.error;
  } else {
    j["entries"] = json::parse(EncodeFileEntries(l.entries));
  }
  return j.dump();
}

std::optional<FileList> DecodeFileList(std::string_view payload) {
  const json j = Parse(payload);
  if (!j.is_object()) return std::nullopt;
  FileList l;
  if (!Get(j, "request", &l.request)) return std::nullopt;
  if (Get(j, "error", &l.error)) return l;
  auto it = j.find("entries");
  if (it == j.end()) return std::nullopt;
  auto entries = DecodeFileEntries(it->dump(), false, kMaxListedEntries);
  if (!entries || entries->empty()) return std::nullopt;
  l.entries = std::move(*entries);
  return l;
}

std::string EncodeFileRecord(const FileEntry& e) {
  const std::string h = EntryJson(e).dump();
  std::string out;
  PutLe32(out, static_cast<uint32_t>(h.size()));
  out += h;
  return out;
}

std::string EncodeFileStreamEnd() {
  const std::string h = json{{"end", true}}.dump();
  std::string out;
  PutLe32(out, static_cast<uint32_t>(h.size()));
  out += h;
  return out;
}

bool FileStreamParser::Fail(std::string why) {
  if (error_.empty()) error_ = std::move(why);
  return false;
}

bool FileStreamParser::Feed(std::string_view bytes) {
  if (!error_.empty()) return false;
  while (!bytes.empty()) {
    if (ended_) return Fail("data after the end of the stream");
    if (in_content_) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(content_left_, bytes.size()));
      if (n > 0 && !h_->OnContent(bytes.substr(0, n))) return Fail("could not write");
      content_left_ -= n;
      bytes.remove_prefix(n);
      if (content_left_ == 0) {
        in_content_ = false;
        if (!h_->OnEntryEnd()) return Fail("could not finish a file");
      }
      continue;
    }
    // A header: 4 bytes of length, then that much JSON.
    const size_t need_len = buf_.size() < 4 ? 4 - buf_.size() : 0;
    if (need_len > 0) {
      const size_t n = std::min(need_len, bytes.size());
      buf_.append(bytes.substr(0, n));
      bytes.remove_prefix(n);
      if (buf_.size() < 4) break;
    }
    const uint32_t len = GetLe32(buf_);
    if (len == 0 || len > kMaxHeaderBytes) return Fail("bad header length");
    const size_t have = buf_.size() - 4;
    const size_t n = std::min<size_t>(len - have, bytes.size());
    buf_.append(bytes.substr(0, n));
    bytes.remove_prefix(n);
    if (buf_.size() - 4 < len) break;
    const json j = Parse(std::string_view(buf_).substr(4));
    buf_.clear();
    if (j.is_object() && j.value("end", false)) {
      ended_ = true;
      h_->OnEnd();
      continue;
    }
    auto e = EntryFromJson(j);
    if (!e || !IsSafeRelativePath(e->path, true)) return Fail("bad entry");
    if (!h_->OnEntry(*e)) return Fail("could not create " + (e->path.empty() ? std::string("the item") : e->path));
    if (e->kind == FileKind::kFile && e->size > e->offset) {
      in_content_ = true;
      content_left_ = e->size - e->offset;
    } else if (!h_->OnEntryEnd()) {
      return Fail("could not finish an entry");
    }
  }
  return true;
}

std::optional<FileEntry> StatLocalFile(const std::string& path, const std::string& name) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return std::nullopt;
  FileEntry e;
  e.path = name;
  e.mode = st.st_mode & 07777;
  e.mtime = static_cast<int64_t>(st.st_mtime);
  if (S_ISREG(st.st_mode)) {
    e.kind = FileKind::kFile;
    e.size = static_cast<uint64_t>(st.st_size);
  } else if (S_ISDIR(st.st_mode)) {
    e.kind = FileKind::kDir;
  } else if (S_ISLNK(st.st_mode)) {
    e.kind = FileKind::kLink;
    char buf[kMaxFilePathBytes + 1];
    const ssize_t n = readlink(path.c_str(), buf, kMaxFilePathBytes);
    if (n <= 0) return std::nullopt;
    e.link.assign(buf, static_cast<size_t>(n));
  } else {
    return std::nullopt;  // sockets, devices, pipes: not copied
  }
  if (e.kind != FileKind::kLink) e.xattrs = ReadXattrs(path);
  return e;
}

namespace {

// Attributes that belong to this computer or to the system, not to the
// file: sandbox and File Provider bookkeeping, compression, SIP.
bool XattrWorthCopying(const std::string& name) {
  static const char* const kSkip[] = {"com.apple.system.", "com.apple.macl", "com.apple.provenance",
                                      "com.apple.rootless", "com.apple.decmpfs", "com.apple.fileprovider",
                                      "com.apple.lastuseddate", "com.apple.metadata:kMDItemDownloadedDate"};
  for (const char* p : kSkip) {
    if (name.rfind(p, 0) == 0) return false;
  }
  return !name.empty() && IsValidUtf8(name) && name.size() <= 127;
}

ssize_t ListXattrs(const std::string& path, char* buf, size_t size) {
#ifdef __APPLE__
  return listxattr(path.c_str(), buf, size, XATTR_NOFOLLOW);
#else
  return llistxattr(path.c_str(), buf, size);
#endif
}

ssize_t GetXattr(const std::string& path, const std::string& name, void* buf, size_t size) {
#ifdef __APPLE__
  return getxattr(path.c_str(), name.c_str(), buf, size, 0, XATTR_NOFOLLOW);
#else
  return lgetxattr(path.c_str(), name.c_str(), buf, size);
#endif
}

}  // namespace

std::map<std::string, std::string> ReadXattrs(const std::string& path) {
  std::map<std::string, std::string> out;
  const ssize_t n = ListXattrs(path, nullptr, 0);
  if (n <= 0) return out;
  std::string names(static_cast<size_t>(n), '\0');
  const ssize_t got = ListXattrs(path, names.data(), names.size());
  if (got <= 0) return out;
  names.resize(static_cast<size_t>(got));
  size_t total = 0;
  for (size_t i = 0; i < names.size();) {
    const size_t end = names.find('\0', i);
    const std::string name = names.substr(i, end == std::string::npos ? std::string::npos : end - i);
    i = end == std::string::npos ? names.size() : end + 1;
    if (!XattrWorthCopying(name)) continue;
    const ssize_t len = GetXattr(path, name, nullptr, 0);
    if (len < 0 || total + name.size() + static_cast<size_t>(len) > kMaxXattrBytes) continue;  // too big: left out
    std::string value(static_cast<size_t>(len), '\0');
    const ssize_t vn = len > 0 ? GetXattr(path, name, value.data(), value.size()) : 0;
    if (vn < 0) continue;
    value.resize(static_cast<size_t>(vn));
    total += name.size() + value.size();
    out[name] = std::move(value);
  }
  return out;
}

void WriteXattrs(const std::string& path, const std::map<std::string, std::string>& xattrs) {
  for (const auto& [name, value] : xattrs) {
    if (!XattrWorthCopying(name)) continue;
#ifdef __APPLE__
    setxattr(path.c_str(), name.c_str(), value.data(), value.size(), 0, XATTR_NOFOLLOW);
#else
    lsetxattr(path.c_str(), name.c_str(), value.data(), value.size(), 0);
#endif
  }
}

bool WalkLocalTree(const std::string& root, size_t max, std::vector<FileEntry>* out, std::string* error) {
  out->clear();
  auto top = StatLocalFile(root, "");
  if (!top) {
    *error = Errno("cannot read " + root);
    return false;
  }
  out->push_back(*top);
  if (top->kind != FileKind::kDir) return true;
  // Depth first, each folder before its contents.
  std::vector<std::string> stack{""};
  while (!stack.empty()) {
    const std::string rel = stack.back();
    stack.pop_back();
    DIR* d = opendir(Join(root, rel).c_str());
    if (!d) {
      *error = Errno("cannot list " + Join(root, rel));
      return false;
    }
    std::vector<std::string> names;
    while (dirent* de = readdir(d)) {
      const std::string n = de->d_name;
      if (n != "." && n != "..") names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    std::vector<std::string> subdirs;
    for (const auto& n : names) {
      const std::string child = rel.empty() ? n : rel + "/" + n;
      auto e = StatLocalFile(Join(root, child), child);
      if (!e) continue;  // gone meanwhile, or not a kind that is copied
      if (out->size() >= max) {
        *error = "more than " + std::to_string(max) + " files";
        return false;
      }
      out->push_back(*e);
      if (e->kind == FileKind::kDir) subdirs.push_back(child);
    }
    // Reverse, so the stack visits them in name order.
    for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(*it);
  }
  return true;
}

// ---- engine ------------------------------------------------------------------

namespace {

// The copying side's last copies, for the whole process: a fetch of an
// earlier copy (a paste under way, or one that goes on after a reconnect)
// is still answered.
struct OfferBook {
  std::mutex mu;
  std::deque<std::pair<uint64_t, std::vector<std::string>>> copies;  // newest last

  void Add(uint64_t announce, std::vector<std::string> paths) {
    std::lock_guard lock(mu);
    copies.emplace_back(announce, std::move(paths));
    while (copies.size() > FileTransfer::kKeptCopies) copies.pop_front();
  }
  std::optional<std::string> Path(uint64_t announce, uint32_t item) {
    std::lock_guard lock(mu);
    for (const auto& [a, paths] : copies) {
      if (a == announce) return item < paths.size() ? std::optional(paths[item]) : std::nullopt;
    }
    return std::nullopt;
  }
};

OfferBook& Offers() {
  static OfferBook* b = new OfferBook;
  return *b;
}

// The pasting side's fetches that a dropped connection cut short, for the
// whole process: the next session's fetch of the same item goes on from
// there.
struct ResumeBook {
  std::mutex mu;
  std::map<std::pair<uint64_t, uint32_t>, FileResumePoint> points;

  void Put(uint64_t key, uint32_t item, FileResumePoint p) {
    std::lock_guard lock(mu);
    points[{key, item}] = std::move(p);
  }
  std::optional<FileResumePoint> Take(uint64_t key, uint32_t item) {
    std::lock_guard lock(mu);
    auto it = points.find({key, item});
    if (it == points.end()) return std::nullopt;
    FileResumePoint p = std::move(it->second);
    points.erase(it);
    return p;
  }
  void Forget(uint64_t key) {
    std::lock_guard lock(mu);
    for (auto it = points.begin(); it != points.end();) {
      it = it->first.first == key ? points.erase(it) : std::next(it);
    }
  }
};

ResumeBook& Resumes() {
  static ResumeBook* b = new ResumeBook;
  return *b;
}

}  // namespace

void RemoveStagedCopy(const std::string& staging_dir, uint64_t announce) {
  Resumes().Forget(announce);
  if (!staging_dir.empty()) RemoveTree(staging_dir + "/" + std::to_string(announce));
}

struct FileTransfer::OutStream {
  uint64_t id = 0;
  std::string root;
  std::vector<FileEntry> entries;
  size_t index = 0;
  // Going on from entries[resume_index] at resume_offset (no resume:
  // resume_index 0, resume_offset 0, which is an ordinary stream).
  size_t resume_index = 0;
  uint64_t resume_offset = 0;
  int fd = -1;
  uint64_t file_left = 0;
  std::string pending;  // a header not sent yet
  bool end_queued = false;
  uint64_t sent = 0, acked = 0;
  bool all_sent = false;
  std::deque<std::pair<uint64_t, uint64_t>> marks;  // (sent up to, at)
  TransferPacer pacer;
  uint64_t congestion_seen = 0;
  uint64_t congestion_checked_us = 0;
  uint64_t reported_us = 0;

  explicit OutStream(const TransferPacer::Options& o) : pacer(o) {}
  ~OutStream() {
    if (fd >= 0) close(fd);
  }

  // Goes on from `from` if it is still what the pasting side has.
  void ResumeFrom(const FileResumePoint& from) {
    for (size_t i = 0; i < entries.size(); ++i) {
      const FileEntry& e = entries[i];
      if (e.path != from.path) continue;
      if (from.file != (e.kind == FileKind::kFile)) return;
      if (from.file && (e.size != from.size || e.mtime != from.mtime || from.offset > e.size)) return;  // changed
      resume_index = i;
      resume_offset = from.file ? from.offset : 0;
      return;
    }
  }

  // Up to `max` bytes of the stream; empty when it is all out. False on a
  // read error (`error` says which).
  bool Next(size_t max, std::string* chunk, std::string* error) {
    chunk->clear();
    while (chunk->size() < max) {
      if (!pending.empty()) {
        const size_t n = std::min(pending.size(), max - chunk->size());
        chunk->append(pending, 0, n);
        pending.erase(0, n);
        continue;
      }
      if (fd >= 0) {
        if (file_left == 0) {
          close(fd);
          fd = -1;
          ++index;
          continue;
        }
        const size_t want = static_cast<size_t>(std::min<uint64_t>(file_left, max - chunk->size()));
        const size_t at = chunk->size();
        chunk->resize(at + want);
        const ssize_t got = read(fd, chunk->data() + at, want);
        if (got <= 0) {
          *error = got < 0 ? Errno("cannot read " + Join(root, entries[index].path))
                           : Join(root, entries[index].path) + " got shorter while it was sent";
          return false;
        }
        chunk->resize(at + static_cast<size_t>(got));
        file_left -= static_cast<uint64_t>(got);
        continue;
      }
      if (index < entries.size()) {
        FileEntry e = entries[index];
        if (e.kind == FileKind::kFile && index < resume_index) {
          // The pasting side has it already.
          e.offset = e.size;
          ++index;
        } else if (e.kind == FileKind::kFile) {
          const std::string path = Join(root, e.path);
          fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
          struct stat st;
          if (fd < 0 || fstat(fd, &st) != 0) {
            *error = Errno("cannot open " + path);
            return false;
          }
          e.size = static_cast<uint64_t>(st.st_size);  // as it is now
          if (index == resume_index && resume_offset > 0 && resume_offset <= e.size) {
            if (lseek(fd, static_cast<off_t>(resume_offset), SEEK_SET) < 0) {
              *error = Errno("cannot seek in " + path);
              return false;
            }
            e.offset = resume_offset;
          }
          file_left = e.size - e.offset;
        } else {
          ++index;
        }
        pending = EncodeFileRecord(e);
        continue;
      }
      if (!end_queued) {
        end_queued = true;
        pending = EncodeFileStreamEnd();
        continue;
      }
      break;
    }
    return true;
  }
};

struct FileTransfer::InStream : FileStreamParser::Handler {
  uint64_t id = 0;
  uint64_t key = 0;
  uint32_t item = 0;
  std::string base;  // where the item itself goes
  FileTarget* target = nullptr;
  FileStreamParser parser{this};
  uint64_t received = 0;
  int fd = -1;
  bool has_current = false;
  FileEntry current;
  std::string current_path;
  uint64_t current_bytes = 0, reported = 0;
  std::vector<std::pair<std::string, FileEntry>> dirs;  // modes and times set at the end

  ~InStream() override {
    if (fd >= 0) close(fd);
  }

  // Where a fetch of the same item goes on from, if this one stops now.
  std::optional<FileResumePoint> ResumePoint() const {
    if (!has_current) return std::nullopt;
    FileResumePoint p;
    p.path = current.path;
    if (current.kind == FileKind::kFile) {
      p.file = true;
      p.size = current.size;
      p.mtime = current.mtime;
      p.offset = fd >= 0 ? current_bytes : current.size;
    }
    return p;
  }

  bool OnEntry(const FileEntry& e) override {
    current = e;
    has_current = true;
    current_path = Join(base, e.path);
    current_bytes = reported = 0;
    const size_t slash = current_path.rfind('/');
    if (slash != std::string::npos && !MakeDirs(current_path.substr(0, slash))) return false;
    switch (e.kind) {
      case FileKind::kDir:
        return MakeDirs(current_path);
      case FileKind::kLink:
        unlink(current_path.c_str());
        return symlink(e.link.c_str(), current_path.c_str()) == 0;
      case FileKind::kFile:
        if (e.offset > 0) {
          // Going on: the first e.offset bytes are here from before.
          fd = open(current_path.c_str(), O_WRONLY | O_CLOEXEC);
          struct stat st;
          if (fd < 0 || fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_size) < e.offset ||
              ftruncate(fd, static_cast<off_t>(e.offset)) != 0 ||
              lseek(fd, static_cast<off_t>(e.offset), SEEK_SET) < 0) {
            return false;
          }
          current_bytes = reported = e.offset;
        } else {
          fd = open(current_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
          if (fd < 0) return false;
        }
        if (e.offset < e.size) target->OnFileStarted(key, item, current, current_path);
        return true;
    }
    return false;
  }

  bool OnContent(std::string_view bytes) override {
    while (!bytes.empty()) {
      const ssize_t n = write(fd, bytes.data(), bytes.size());
      if (n <= 0) return false;
      bytes.remove_prefix(static_cast<size_t>(n));
      current_bytes += static_cast<uint64_t>(n);
    }
    if (current_bytes - reported >= kProgressEveryBytes) {
      reported = current_bytes;
      target->OnProgress(key, item, current.path, current_bytes);
    }
    return true;
  }

  bool OnEntryEnd() override {
    if (fd >= 0) {
      const int f = fd;
      fd = -1;
      if (close(f) != 0) return false;
    }
    if (current.kind == FileKind::kDir) {
      dirs.emplace_back(current_path, current);
    } else if (current.kind == FileKind::kFile) {
      chmod(current_path.c_str(), (current.mode & 07777) | 0600);
      WriteXattrs(current_path, current.xattrs);
      struct timeval tv[2] = {{static_cast<time_t>(current.mtime), 0}, {static_cast<time_t>(current.mtime), 0}};
      utimes(current_path.c_str(), tv);
    }
    if (current.kind != FileKind::kDir) target->OnFileReady(key, item, current, current_path);
    return true;
  }

  void OnEnd() override {
    // Folders last (deepest first), so their times are not touched by
    // what was written into them, and they were writable meanwhile.
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
      WriteXattrs(it->first, it->second.xattrs);
      chmod(it->first.c_str(), (it->second.mode & 07777) | 0700);
      struct timeval tv[2] = {{static_cast<time_t>(it->second.mtime), 0}, {static_cast<time_t>(it->second.mtime), 0}};
      utimes(it->first.c_str(), tv);
      target->OnFileReady(key, item, it->second, it->first);
    }
  }
};

FileTransfer::FileTransfer(SendFn send, Options options) : send_(std::move(send)), options_(std::move(options)) {
  // Keys for an older peer's copies: its ids with the top bit set and
  // mixed with this session's own number, so they meet neither a newer
  // peer's (below 2^53) nor another session's.
  salt_ = NewCopyId() | (1ull << 63);
  if (options_.target) options_.target->SetRequester(this);
}

FileTransfer::~FileTransfer() { Stop(); }

void FileTransfer::Stop() {
  // Fetches under way: what arrived stays in the staging folder, and the
  // target asks the next session for the rest.
  for (auto& [id, in] : ins_) {
    if (auto p = in->ResumePoint()) Resumes().Put(in->key, in->item, std::move(*p));
  }
  ins_.clear();
  fetching_.clear();
  if (options_.target) {
    options_.target->DropRequester(this);
    for (const auto& [id, list] : lists_) options_.target->OnList(list.first, list.second, {}, "not connected");
    if (published_) options_.target->Withdraw(peer_key_);
  }
  lists_.clear();
  published_ = false;
  out_.reset();
  fetches_.clear();
  peer_key_ = 0;
}

uint64_t FileTransfer::Now() const { return options_.clock ? options_.clock() : SteadyUs(); }

void FileTransfer::Log(const std::string& line) {
  if (options_.log) options_.log("files: " + line);
}

std::vector<FileEntry> FileTransfer::Describe(const std::vector<std::string>& local_paths,
                                              std::vector<std::string>* kept) {
  std::vector<FileEntry> out;
  kept->clear();
  for (const auto& p : local_paths) {
    if (out.size() >= kMaxAnnouncedFiles) break;
    std::string name = p;
    while (name.size() > 1 && name.back() == '/') name.pop_back();
    const size_t slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (!IsSafeFileName(name)) continue;
    if (auto e = StatLocalFile(p, name)) {
      out.push_back(*e);
      kept->push_back(p);
    }
  }
  return out;
}

// ---- copying side ------------------------------------------------------------

void FileTransfer::SetOffered(uint64_t announce, std::vector<std::string> local_paths) {
  // What is being sent of earlier copies goes on: the other side may be
  // pasting them.
  if (!local_paths.empty()) Offers().Add(announce, std::move(local_paths));
}

void FileTransfer::HandleListRequest(const std::string& payload) {
  auto r = DecodeFileListRequest(payload);
  if (!r) return;
  FileList l;
  l.request = r->id;
  const auto path = Offers().Path(r->announce, r->item);
  if (!path) {
    l.error = "gone";
  } else if (!WalkLocalTree(*path, kMaxListedEntries, &l.entries, &l.error)) {
    Log(l.error);
    l.entries.clear();
  }
  send_(kFileListMessageType, EncodeFileList(l));
}

void FileTransfer::HandleFetch(const std::string& payload) {
  auto f = DecodeFileFetch(payload);
  if (!f) return;
  fetches_.push_back(*f);
  StartNextStream();
}

void FileTransfer::StartNextStream() {
  while (!out_ && !fetches_.empty()) {
    const FileFetch f = fetches_.front();
    fetches_.pop_front();
    const auto path = Offers().Path(f.announce, f.item);
    if (!path) {
      send_(kFileCancelMessageType, EncodeClipboardCancel({f.id, true, "gone"}));
      continue;
    }
    auto out = std::make_unique<OutStream>(options_.pacer);
    out->id = f.id;
    out->root = *path;
    std::string error;
    if (!WalkLocalTree(out->root, kMaxListedEntries, &out->entries, &error)) {
      Log(error);
      send_(kFileCancelMessageType, EncodeClipboardCancel({f.id, true, "error"}));
      continue;
    }
    if (f.from) out->ResumeFrom(*f.from);
    if (out->resume_index > 0 || out->resume_offset > 0) {
      Log("sending " + out->root + ", going on from " + (f.from->path.empty() ? "the start" : f.from->path) + " at " +
          std::to_string(out->resume_offset));
    } else {
      Log("sending " + out->root);
    }
    out_ = std::move(out);
  }
}

bool FileTransfer::SendData(std::string_view payload) {
  return options_.send_bulk ? options_.send_bulk(kFileDataMessageType, payload) : send_(kFileDataMessageType, payload);
}

uint64_t FileTransfer::PumpBulk(uint64_t now, int64_t queued) {
  // Enough waiting in the transport to keep a fast path busy between two
  // refills, little enough that a cancel or a new copy is not stuck behind
  // seconds of data. The transport's own congestion control sets the speed
  // and puts the picture first.
  constexpr int64_t kTarget = 1 << 20;
  constexpr uint64_t kMaxUnacked = 64ull << 20;  // the receiver keeps up
  constexpr uint64_t kRefillUs = 10'000;
  OutStream& o = *out_;
  if (now - o.reported_us >= 5'000'000) {
    char line[160];
    snprintf(line, sizeof line, "sending on the bulk channel, %lld KiB queued, %llu KiB unacknowledged, %.1f MB sent",
             static_cast<long long>(queued / 1024), static_cast<unsigned long long>((o.sent - o.acked) / 1024),
             o.sent / 1e6);
    Log(line);
    o.reported_us = now;
  }
  std::string chunk;
  while (!o.all_sent && queued < kTarget && o.sent - o.acked < kMaxUnacked) {
    std::string error;
    if (!o.Next(kChunk, &chunk, &error)) {
      Log(error);
      DropOutgoing("error", true);
      StartNextStream();
      return Now();
    }
    if (chunk.empty()) {
      o.all_sent = true;
      if (o.acked == o.sent) {
        Log("sent " + o.root + " (" + std::to_string(o.sent) + " bytes)");
        out_.reset();
        StartNextStream();
        return Now();
      }
      break;
    }
    if (!SendData(EncodeClipboardData({o.id, o.sent}, chunk))) {
      Log("sending stopped: the connection is gone");
      out_.reset();
      fetches_.clear();
      return now + 1'000'000;
    }
    o.sent += chunk.size();
    queued += static_cast<int64_t>(chunk.size());
  }
  return o.all_sent ? now + 1'000'000 : now + kRefillUs;  // all sent: acknowledgements wake the thread
}

uint64_t FileTransfer::Pump(uint64_t now) {
  const uint64_t idle = now + 1'000'000;
  if (!out_) return idle;
  OutStream& o = *out_;
  if (o.reported_us == 0) o.reported_us = now;
  if (options_.bulk_queued) {
    const int64_t queued = options_.bulk_queued();
    if (queued >= 0) return PumpBulk(now, queued);
  }
  if (options_.congestion_events && now - o.congestion_checked_us >= kCongestionEveryUs) {
    o.congestion_checked_us = now;
    const uint64_t c = options_.congestion_events();
    if (o.congestion_seen != 0 && c > o.congestion_seen) o.pacer.OnCongestion(now);
    o.congestion_seen = std::max<uint64_t>(c, 1);
  }
  if (now - o.reported_us >= 5'000'000) {
    // What holds a slow transfer back: the rate, the window or the path.
    char line[160];
    snprintf(line, sizeof line, "sending at %.1f Mbps, %llu KiB in flight (window %llu KiB), base delay %llu ms, %.1f MB sent",
             o.pacer.rate_bps() / 1e6, static_cast<unsigned long long>((o.sent - o.acked) / 1024),
             static_cast<unsigned long long>(o.pacer.Window() / 1024),
             static_cast<unsigned long long>(o.pacer.base_delay_us() / 1000), o.sent / 1e6);
    Log(line);
    o.reported_us = now;
  }
  std::string chunk;
  while (!o.all_sent) {
    if (!o.pacer.CanSend(now, kChunk, o.sent - o.acked)) {
      if (o.sent - o.acked + kChunk > o.pacer.Window()) return idle;  // waits for acknowledgements
      return o.pacer.NextSendUs(now, kChunk);
    }
    std::string error;
    if (!o.Next(kChunk, &chunk, &error)) {
      Log(error);
      DropOutgoing("error", true);
      StartNextStream();
      return Now();
    }
    if (chunk.empty()) {
      o.all_sent = true;
      if (o.acked == o.sent) {  // acknowledged already
        Log("sent " + o.root + " (" + std::to_string(o.sent) + " bytes)");
        out_.reset();
        StartNextStream();
        return Now();
      }
      break;
    }
    if (!SendData(EncodeClipboardData({o.id, o.sent}, chunk))) {
      Log("sending stopped: the connection is gone");
      out_.reset();
      fetches_.clear();
      return idle;
    }
    o.pacer.OnSent(now, chunk.size());
    o.sent += chunk.size();
    o.marks.emplace_back(o.sent, now);
    now = Now();
  }
  return idle;  // all sent: acknowledgements wake the thread
}

void FileTransfer::HandleAck(const std::string& payload, uint64_t now) {
  auto a = DecodeClipboardAck(payload);
  if (!a || !out_ || a->id != out_->id) return;
  OutStream& o = *out_;
  if (a->bytes > o.sent || a->bytes < o.acked) return;
  uint64_t at = 0;
  while (!o.marks.empty() && o.marks.front().first <= a->bytes) {
    at = o.marks.front().second;
    o.marks.pop_front();
  }
  if (at != 0) o.pacer.OnAck(now, now - at);
  o.acked = a->bytes;
  if (o.all_sent && o.acked == o.sent) {
    Log("sent " + o.root + " (" + std::to_string(o.sent) + " bytes)");
    out_.reset();
    StartNextStream();
  }
}

void FileTransfer::DropOutgoing(const std::string& reason, bool tell_peer) {
  if (!out_) return;
  if (tell_peer) send_(kFileCancelMessageType, EncodeClipboardCancel({out_->id, true, reason}));
  out_.reset();
}

// ---- pasting side --------------------------------------------------------------

std::string FileTransfer::ItemDir(uint64_t key, uint32_t item) const {
  return options_.staging_dir + "/" + std::to_string(key) + "/" + std::to_string(item);
}

uint64_t FileTransfer::WireAnnounce(uint64_t key) const {
  auto it = salted_.find(key);
  return it == salted_.end() ? key : it->second;
}

std::optional<std::vector<std::string>> FileTransfer::OnPeerAnnounce(uint64_t announce,
                                                                     const std::vector<FileEntry>& items,
                                                                     bool unique_ids) {
  Withdraw();
  if (!options_.target || items.empty()) return std::nullopt;
  uint64_t key = announce;
  if (!unique_ids) {
    key = announce ^ salt_;
    salted_[key] = announce;
  }
  peer_key_ = key;
  auto paths = options_.target->Publish(key, items);
  published_ = paths.has_value();
  names_only_ = !published_;
  return paths;
}

void FileTransfer::Withdraw() {
  if (peer_key_ == 0) return;
  if (published_ && options_.target) options_.target->Withdraw(peer_key_);
  published_ = false;
  names_only_ = false;
  peer_key_ = 0;
}

void FileTransfer::NamesPasted() {
  if (!names_only_ || !options_.target) return;
  names_only_ = false;  // once per copy
  options_.target->OnNamesPasted();
}

void FileTransfer::RequestFileList(uint64_t announce, uint32_t item) {
  if (options_.post) options_.post(kPostList, announce, item, {});
}

void FileTransfer::RequestFileFetch(uint64_t announce, uint32_t item, const std::string& name) {
  if (options_.post) options_.post(kPostFetch, announce, item, name);
}

void FileTransfer::OnPosted(uint32_t kind, uint64_t key, uint32_t item, const std::string& name) {
  FileTarget* t = options_.target.get();
  if (!t || key == 0) return;
  const uint64_t announce = WireAnnounce(key);
  if (kind == kPostList) {
    const uint64_t id = ++next_id_;
    if (!send_(kFileListRequestMessageType, EncodeFileListRequest({announce, id, item}))) {
      t->OnList(key, item, {}, "not connected");
      return;
    }
    lists_[id] = {key, item};
    return;
  }
  if (kind != kPostFetch || fetching_.count({key, item})) return;  // asked already
  if (!IsSafeFileName(name) || options_.staging_dir.empty()) {
    t->OnItemDone(key, item, "cannot write to the staging folder");
    return;
  }
  const uint64_t id = ++next_id_;
  auto in = std::make_unique<InStream>();
  in->id = id;
  in->key = key;
  in->item = item;
  in->target = t;
  const std::string dir = ItemDir(key, item);
  FileFetch f{announce, id, item, Resumes().Take(key, item)};
  if (!f.from) RemoveTree(dir);  // a fetch that failed before
  if (!MakeDirs(dir)) {
    t->OnItemDone(key, item, "cannot write to the staging folder");
    return;
  }
  in->base = dir + "/" + name;
  if (!send_(kFileFetchMessageType, EncodeFileFetch(f))) {
    if (f.from) Resumes().Put(key, item, *f.from);  // for the next try
    t->OnItemDone(key, item, "not connected");
    return;
  }
  if (f.from) {
    Log("fetching " + name + ", going on from " + (f.from->path.empty() ? name : f.from->path));
  } else {
    Log("fetching " + name);
  }
  fetching_.insert({key, item});
  ins_[id] = std::move(in);
}

void FileTransfer::HandleList(const std::string& payload) {
  auto l = DecodeFileList(payload);
  if (!l) {
    Log("ignored a malformed list from the peer");
    return;
  }
  auto it = lists_.find(l->request);
  if (it == lists_.end()) return;
  const auto [key, item] = it->second;
  lists_.erase(it);
  if (options_.target) options_.target->OnList(key, item, l->entries, l->error);
}

void FileTransfer::HandleData(const std::string& payload) {
  std::string_view bytes;
  auto h = DecodeClipboardData(payload, &bytes);
  if (!h) return;
  auto it = ins_.find(h->id);
  if (it == ins_.end()) return;  // a cancelled fetch's, late
  InStream& in = *it->second;
  if (h->offset != in.received || bytes.empty()) {
    FinishIncoming(h->id, "the peer sent data out of order", true);
    return;
  }
  in.received += bytes.size();
  if (!in.parser.Feed(bytes)) {
    FinishIncoming(h->id, in.parser.error(), true);
    return;
  }
  send_(kFileAckMessageType, EncodeClipboardAck({in.id, in.received}));
  if (in.parser.ended()) FinishIncoming(h->id, "", false);
}

void FileTransfer::HandleCancel(const std::string& payload) {
  auto c = DecodeClipboardCancel(payload);
  if (!c) return;
  if (c->from_sender) {
    if (ins_.count(c->id)) FinishIncoming(c->id, c->reason.empty() ? "cancelled" : c->reason, false);
    return;
  }
  // The peer gave up a fetch of ours to send.
  if (out_ && out_->id == c->id) {
    DropOutgoing(c->reason, false);
    StartNextStream();
  }
  fetches_.erase(std::remove_if(fetches_.begin(), fetches_.end(), [&](const FileFetch& f) { return f.id == c->id; }),
                 fetches_.end());
}

void FileTransfer::FinishIncoming(uint64_t id, const std::string& error, bool tell_peer) {
  auto it = ins_.find(id);
  if (it == ins_.end()) return;
  std::unique_ptr<InStream> in = std::move(it->second);
  ins_.erase(it);
  if (tell_peer) send_(kFileCancelMessageType, EncodeClipboardCancel({id, false, "error"}));
  fetching_.erase({in->key, in->item});
  if (!error.empty()) Log("fetching " + in->base + " failed: " + error);
  const uint64_t key = in->key;
  const uint32_t item = in->item;
  in.reset();  // closes the file being written
  if (options_.target) options_.target->OnItemDone(key, item, error);
}

void FileTransfer::Handle(uint32_t type, const std::string& payload, uint64_t now) {
  switch (type) {
    case kFileListRequestMessageType:
      HandleListRequest(payload);
      return;
    case kFileListMessageType:
      HandleList(payload);
      return;
    case kFileFetchMessageType:
      HandleFetch(payload);
      return;
    case kFileDataMessageType:
      HandleData(payload);
      return;
    case kFileAckMessageType:
      HandleAck(payload, now);
      return;
    case kFileCancelMessageType:
      HandleCancel(payload);
      return;
  }
}

}  // namespace rdm
