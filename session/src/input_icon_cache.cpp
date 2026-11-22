#include <dispatch/dispatch.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

#include "rdmachine/ime_wire.h"
#include "rdmachine/session/client_input_icon.h"

namespace rdm {

namespace fs = std::filesystem;

namespace {

bool IsPng(const std::vector<uint8_t>& b) {
  static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  return b.size() > sizeof kSignature && std::equal(std::begin(kSignature), std::end(kSignature), b.begin());
}

// What can be kept: a PNG, not too large, that is what its fingerprint says.
bool Valid(const std::string& fingerprint, const std::vector<uint8_t>& png) {
  return IsImeIconFingerprint(fingerprint) && png.size() <= kImeIconMaxBytes && IsPng(png) &&
         ImeIconFingerprint(png) == fingerprint;
}

struct Write {
  std::string dir, fingerprint;
  std::vector<uint8_t> png;
  size_t max_files;
};

// On the cache's queue: the file, then the oldest beyond the bound go
// (icons are few: this is only a bound).
void WriteToDisk(void* context) {
  std::unique_ptr<Write> w(static_cast<Write*>(context));
  std::error_code ec;
  fs::create_directories(w->dir, ec);
  const fs::path path = fs::path(w->dir) / (w->fingerprint + ".png");
  const fs::path tmp = fs::path(path).concat(".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(w->png.data()), static_cast<std::streamsize>(w->png.size()));
    if (!out) {
      out.close();
      fs::remove(tmp, ec);
      return;  // kept in memory at least
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return;
  }
  std::vector<fs::directory_entry> files;
  for (const auto& e : fs::directory_iterator(w->dir, ec)) {
    if (e.is_regular_file(ec) && e.path().extension() == ".png") files.push_back(e);
  }
  if (files.size() <= w->max_files) return;
  std::sort(files.begin(), files.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
    std::error_code e1, e2;
    return a.last_write_time(e1) < b.last_write_time(e2);
  });
  for (size_t i = 0; i + w->max_files < files.size(); ++i) {
    if (files[i].path() != path) fs::remove(files[i].path(), ec);
  }
}

}  // namespace

InputIconCache::InputIconCache(std::string dir, size_t max_files)
    : dir_(std::move(dir)), max_files_(max_files),
      queue_(dispatch_queue_create("org.rdmachine.input-icons", DISPATCH_QUEUE_SERIAL)) {
  if (dir_.empty()) return;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir_, ec)) {
    if (memory_.size() >= max_files_) break;
    if (!e.is_regular_file(ec) || e.path().extension() != ".png") continue;
    const std::string fingerprint = e.path().stem().string();
    if (!IsImeIconFingerprint(fingerprint)) continue;
    std::ifstream in(e.path(), std::ios::binary);
    std::vector<uint8_t> png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (Valid(fingerprint, png)) memory_[fingerprint] = std::move(png);  // a damaged one is asked for again
  }
}

InputIconCache::~InputIconCache() {
  Flush();
  dispatch_release(static_cast<dispatch_queue_t>(queue_));
}

void InputIconCache::Remember(const std::string& fingerprint, std::vector<uint8_t> png) {
  if (memory_.size() >= max_files_ && !memory_.count(fingerprint)) memory_.erase(memory_.begin());
  memory_[fingerprint] = std::move(png);
}

std::optional<std::vector<uint8_t>> InputIconCache::Get(const std::string& fingerprint) {
  std::lock_guard lock(mu_);
  if (auto it = memory_.find(fingerprint); it != memory_.end()) return it->second;
  return std::nullopt;
}

bool InputIconCache::Put(const std::string& fingerprint, const std::vector<uint8_t>& png) {
  if (!Valid(fingerprint, png)) return false;  // also never a path of its own
  {
    std::lock_guard lock(mu_);
    Remember(fingerprint, png);
  }
  if (!dir_.empty()) {
    dispatch_async_f(static_cast<dispatch_queue_t>(queue_), new Write{dir_, fingerprint, png, max_files_},
                     &WriteToDisk);
  }
  return true;
}

void InputIconCache::Flush() {
  dispatch_sync_f(static_cast<dispatch_queue_t>(queue_), nullptr, [](void*) {});
}

std::string InputIconCacheDir() {
  const passwd* pw = getpwuid(getuid());
  if (!pw || !pw->pw_dir || !*pw->pw_dir) return {};
  return std::string(pw->pw_dir) + "/Library/Caches/org.rdmachine.server/input-icons";
}

}  // namespace rdm
