#include "rdmachine/clipboard_files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <set>
#include <system_error>

#include "rdmachine/random.h"

namespace fs = std::filesystem;

namespace rdm {
namespace {

std::string Errno(const std::string& what) { return what + ": " + std::strerror(errno); }

int64_t MtimeOf(const struct stat& st) {
#ifdef __APPLE__
  return static_cast<int64_t>(st.st_mtimespec.tv_sec);
#else
  return static_cast<int64_t>(st.st_mtim.tv_sec);
#endif
}

struct Walker {
  const FileListLimits& limits;
  ClipboardOffer* offer;
  std::vector<std::string>* sources;
  uint64_t bytes = 0;
  uint64_t files = 0;
  bool over = false;
  std::string reason;

  // `rel` is the entry's path in the offer.
  void Add(const fs::path& abs, const std::string& rel) {
    if (over && offer->entries.size() > 10 * limits.max_entries) return;  // counting stops here
    struct stat st {};
    if (lstat(abs.c_str(), &st) != 0) return;  // gone meanwhile
    OfferEntry e;
    e.path = rel;
    e.mode = static_cast<uint32_t>(st.st_mode & 0777);
    e.mtime = MtimeOf(st);
    if (S_ISREG(st.st_mode)) {
      e.kind = OfferEntry::Kind::kFile;
      e.size = static_cast<uint64_t>(st.st_size);
      bytes += e.size;
      ++files;
      if (!over) sources->push_back(abs.string());
    } else if (S_ISDIR(st.st_mode)) {
      e.kind = OfferEntry::Kind::kDir;
    } else if (S_ISLNK(st.st_mode)) {
      e.kind = OfferEntry::Kind::kLink;
      std::error_code ec;
      e.target = fs::read_symlink(abs, ec).string();
      if (ec || e.target.empty()) return;
    } else {
      return;  // sockets, pipes, devices
    }
    offer->entries.push_back(e);
    if (!over && bytes > limits.max_bytes) {
      over = true;
      reason = limits.max_bytes == 0 ? "not_accepted" : "too_large";
    }
    if (!over && offer->entries.size() > limits.max_entries) {
      over = true;
      reason = "too_many";
    }
    if (e.kind != OfferEntry::Kind::kDir) return;
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(abs, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
      names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
      if (!IsSafeRelativePath(n)) continue;  // not representable (control characters, ...)
      Add(abs / n, rel + "/" + n);
    }
  }
};

}  // namespace

bool ListFiles(const std::vector<std::string>& roots, const FileListLimits& limits, ClipboardOffer* offer,
               std::vector<std::string>* sources, std::string* error) {
  offer->entries.clear();
  sources->clear();
  Walker w{limits, offer, sources, 0, 0, false, {}};
  std::set<std::string> names;
  for (const auto& r : roots) {
    fs::path p(r);
    while (!p.empty() && p.filename().empty()) p = p.parent_path();  // "dir/"
    std::string name = p.filename().string();
    if (name.empty() || !IsSafeRelativePath(name) || name.find('/') != std::string::npos) continue;
    // Two roots of the same name (from different folders): number the later.
    if (names.count(name)) {
      const fs::path n(name);
      for (int k = 2;; ++k) {
        std::string alt = n.stem().string() + " " + std::to_string(k) + n.extension().string();
        if (!names.count(alt)) {
          name = alt;
          break;
        }
      }
    }
    const size_t before = offer->entries.size();
    w.Add(p, name);
    if (offer->entries.size() > before) names.insert(name);
  }
  if (offer->entries.empty()) {
    if (error) *error = "none of the copied files exists any more";
    return false;
  }
  if (w.over) {
    offer->skipped_files = w.files;
    offer->skipped_bytes = w.bytes;
    offer->skipped_reason = w.reason;
    offer->entries.clear();
    sources->clear();
  }
  return true;
}

std::unique_ptr<StagingDir> StagingDir::Create(const std::string& root, const ClipboardOffer& offer,
                                               std::string* error) {
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec) {
    *error = "cannot create " + root + ": " + ec.message();
    return nullptr;
  }
  chmod(root.c_str(), 0700);
  uint8_t rnd[6];
  if (!RandomBytes(rnd)) {
    *error = "no random source";
    return nullptr;
  }
  static const char* kHex = "0123456789abcdef";
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  std::string name = std::to_string(secs) + "-";
  for (uint8_t b : rnd) {
    name.push_back(kHex[b >> 4]);
    name.push_back(kHex[b & 15]);
  }
  std::unique_ptr<StagingDir> d(new StagingDir());
  d->offer_ = offer;
  d->path_ = (fs::path(root) / name).string();
  if (mkdir(d->path_.c_str(), 0700) != 0) {
    *error = Errno("cannot create " + d->path_);
    return nullptr;
  }
  for (const auto& e : offer.entries) {
    const std::string p = d->path_ + "/" + e.path;
    if (e.kind == OfferEntry::Kind::kDir) {
      if (mkdir(p.c_str(), 0700) != 0) {
        *error = Errno("cannot create " + p);
        d->Remove();
        return nullptr;
      }
    } else if (e.kind == OfferEntry::Kind::kLink) {
      if (symlink(e.target.c_str(), p.c_str()) != 0) {
        *error = Errno("cannot create link " + p);
        d->Remove();
        return nullptr;
      }
    }
  }
  return d;
}

StagingDir::~StagingDir() {
  if (fd_ >= 0) close(fd_);
}

bool StagingDir::Open(size_t entry, std::string* error) {
  if (fd_ >= 0 || entry >= offer_.entries.size() || offer_.entries[entry].kind != OfferEntry::Kind::kFile) {
    *error = "internal: bad file entry";
    return false;
  }
  const std::string p = path_ + "/" + offer_.entries[entry].path;
  // Every parent is a directory made by Create (ValidateOffer), and the
  // staging directory is private: O_EXCL | O_NOFOLLOW only make sure.
  fd_ = open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    *error = Errno("cannot create " + p);
    return false;
  }
  entry_ = entry;
  return true;
}

bool StagingDir::Append(std::string_view bytes, std::string* error) {
  while (!bytes.empty()) {
    const ssize_t n = write(fd_, bytes.data(), bytes.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      *error = Errno("cannot write " + offer_.entries[entry_].path);
      return false;
    }
    bytes.remove_prefix(static_cast<size_t>(n));
  }
  return true;
}

bool StagingDir::Close(std::string* error) {
  const OfferEntry& e = offer_.entries[entry_];
  if (e.mode != 0) fchmod(fd_, static_cast<mode_t>((e.mode & 0777) | 0600));
  if (e.mtime != 0) {
    struct timespec ts[2] = {{0, UTIME_OMIT}, {static_cast<time_t>(e.mtime), 0}};
    futimens(fd_, ts);
  }
  const int r = close(fd_);
  fd_ = -1;
  if (r != 0) {
    *error = Errno("cannot write " + e.path);
    return false;
  }
  return true;
}

void StagingDir::Finish() {
  // Deepest first, so setting a parent's time comes after its contents.
  for (auto it = offer_.entries.rbegin(); it != offer_.entries.rend(); ++it) {
    const std::string p = path_ + "/" + it->path;
    if (it->kind == OfferEntry::Kind::kDir) {
      chmod(p.c_str(), static_cast<mode_t>((it->mode != 0 ? it->mode & 0777 : 0755) | 0700));
    }
    if (it->mtime != 0) {
      struct timespec ts[2] = {{0, UTIME_OMIT}, {static_cast<time_t>(it->mtime), 0}};
      utimensat(AT_FDCWD, p.c_str(), ts, AT_SYMLINK_NOFOLLOW);
    }
  }
}

std::vector<std::string> StagingDir::RootPaths() const {
  std::vector<std::string> out;
  for (const auto& e : offer_.entries) {
    if (e.path.find('/') == std::string::npos) out.push_back(path_ + "/" + e.path);
  }
  return out;
}

void StagingDir::Remove() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  std::error_code ec;
  // Directories may have lost their write permission (Finish): give it back.
  for (fs::recursive_directory_iterator it(path_, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec) && !it->is_symlink(ec)) chmod(it->path().c_str(), 0700);
  }
  fs::remove_all(path_, ec);
}

void RemoveStagingDirs(const std::string& root, const std::string& keep) {
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().string() != keep) dirs.push_back(it->path());
  }
  for (const auto& d : dirs) {
    for (fs::recursive_directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_directory(ec) && !it->is_symlink(ec)) chmod(it->path().c_str(), 0700);
    }
    ec.clear();
    fs::remove_all(d, ec);
  }
}

std::string StagingDirOf(const std::string& root, const std::string& path) {
  if (root.empty()) return {};
  fs::path r = fs::path(root).lexically_normal();
  if (!r.has_filename()) r = r.parent_path();  // "root/"
  const fs::path p = fs::path(path).lexically_normal();
  auto ri = r.begin();
  auto pi = p.begin();
  for (; ri != r.end(); ++ri, ++pi) {
    if (pi == p.end() || *ri != *pi) return {};
  }
  if (pi == p.end()) return {};
  return (r / *pi).string();
}

}  // namespace rdm
