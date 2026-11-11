#pragma once
// Files on the clipboard (docs/protocol.md, "剪贴板"): the sender lists what
// was copied, the receiver rebuilds it in a staging directory of its own and
// puts that on its clipboard, so pasting is an ordinary local copy.
//
// Platform independent (POSIX file calls and std::filesystem).
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rdmachine/clipboard_wire.h"

namespace rdm {

struct FileListLimits {
  uint64_t max_bytes = 0;    // file data; more and the files are skipped
  size_t max_entries = 10000;
};

// Lists `roots` (absolute paths copied in a file manager) into offer->entries,
// directories recursively, symbolic links as links (never followed), other
// special files left out. sources gets the absolute path of each file entry,
// in item order. Over a limit, offer->entries stays empty and offer->skipped_*
// say what was left out (the bytes counted so far, so at least that much).
// Roots that no longer exist are left out; false if none is left.
bool ListFiles(const std::vector<std::string>& roots, const FileListLimits& limits, ClipboardOffer* offer,
               std::vector<std::string>* sources, std::string* error);

// Where one received offer is rebuilt: <root>/<unique name>/. The directory
// and its parents are private to the user (0700). Not thread-safe.
class StagingDir {
 public:
  // Makes the directory and every directory and link of the offer (which
  // must have passed ValidateOffer). Null with *error on failure.
  static std::unique_ptr<StagingDir> Create(const std::string& root, const ClipboardOffer& offer, std::string* error);
  ~StagingDir();  // closes an open file; keeps what is on disk

  StagingDir(const StagingDir&) = delete;
  StagingDir& operator=(const StagingDir&) = delete;

  // Files, by their index in offer.entries, one at a time: Open, Append
  // their bytes in order, Close (which sets permissions and time).
  bool Open(size_t entry, std::string* error);
  bool Append(std::string_view bytes, std::string* error);
  bool Close(std::string* error);
  // After the last file: the directories' permissions and times.
  void Finish();

  // Absolute paths of the top-level entries: what goes on the clipboard.
  std::vector<std::string> RootPaths() const;
  const std::string& path() const { return path_; }
  // Deletes the directory with everything in it.
  void Remove();

 private:
  StagingDir() = default;

  ClipboardOffer offer_;
  std::string path_;
  int fd_ = -1;
  size_t entry_ = 0;
};

// Deletes every staging directory under `root` except `keep` (a path
// returned by StagingDir::path(), or empty). Quietly skips what it cannot.
void RemoveStagingDirs(const std::string& root, const std::string& keep);

// Whether `path` lies inside a staging directory under `root`; returns that
// directory (empty if not).
std::string StagingDirOf(const std::string& root, const std::string& path);

}  // namespace rdm
