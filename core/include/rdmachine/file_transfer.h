#pragma once
// Files and folders copied on one side and pasted on the other
// (docs/file-paste.md). Part of the clipboard: an announcement lists the
// copied items (name, size, kind), nothing else; the pasting side lists a
// folder's tree or fetches an item only when the system there asks for it
// (macOS: Finder copying the File Provider placeholders the clipboard points
// at).
//
// Messages, on the picture connection next to the clipboard's:
//   list request (pasting side)  {"announce":3,"id":1,"item":0}
//   list         (copying side)  {"request":1,"entries":[...]} or {"request":1,"error":"..."}
//   fetch        (pasting side)  {"announce":3,"id":2,"item":0}: send item 0, all of it
//   data         (copying side)  the item as one stream (below): id, offset, bytes
//   ack          (pasting side)  id, bytes written
//   cancel       (either side)   {"id":2,"from_sender":true,"reason":"error"}
// data, ack and cancel have the clipboard's encoding (clipboard_wire.h)
// under their own types; `id` is the fetch's.
//
// The stream of one item is a sequence of records, like a tar file:
//   u32 (little endian) length of the header, the header (JSON), then
//   `size` bytes of content for a regular file.
// Header: {"p":"sub/a.txt","k":"f","s":12,"m":420,"t":1700000000} with
// k = "f" (file), "d" (folder) or "l" (symbolic link, "l":"target"); "p" is
// relative to the item ("" = the item itself). The last record is {"end":true}.
//
// Platform independent: plain POSIX files (macOS and Linux).
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rdmachine/transfer_pacer.h"

namespace rdm {

// rdaccel application message types (RDA_MSG_USER_FIRST + 12 ... + 17).
constexpr uint32_t kFileListRequestMessageType = 0x1000C;
constexpr uint32_t kFileListMessageType = 0x1000D;
constexpr uint32_t kFileFetchMessageType = 0x1000E;
constexpr uint32_t kFileDataMessageType = 0x1000F;
constexpr uint32_t kFileAckMessageType = 0x10010;
constexpr uint32_t kFileCancelMessageType = 0x10011;

bool IsFileMessage(uint32_t type);

// Limits, both ways.
constexpr size_t kMaxAnnouncedFiles = 1000;      // top-level items of one copy
constexpr size_t kMaxListedEntries = 100000;     // one item's tree
constexpr size_t kMaxFilePathBytes = 4096;

enum class FileKind { kFile, kDir, kLink };

// One file, folder or link. In an announcement `path` is the item's name; in
// a list or a stream it is relative to the item ("" = the item itself).
struct FileEntry {
  std::string path;
  FileKind kind = FileKind::kFile;
  uint64_t size = 0;  // regular files
  uint32_t mode = 0;  // permission bits (07777)
  int64_t mtime = 0;  // seconds since 1970
  std::string link;   // a link's target

  bool operator==(const FileEntry&) const = default;
};

// A path the peer sent that is safe to put under a folder of ours: relative,
// no "." or ".." parts, no empty parts, no NUL, valid UTF-8. `allow_empty`:
// "" (the item itself) is fine.
bool IsSafeRelativePath(std::string_view p, bool allow_empty);
// An item name: one safe part.
bool IsSafeFileName(std::string_view name);

// ---- wire ------------------------------------------------------------------

// A JSON array of entries (for the clipboard announcement and the list).
// `names_only`: announcement entries, whose path must be a single name.
std::string EncodeFileEntries(const std::vector<FileEntry>& entries);
std::optional<std::vector<FileEntry>> DecodeFileEntries(std::string_view json_array, bool names_only, size_t max);

struct FileListRequest {
  uint64_t announce = 0;
  uint64_t id = 0;  // the pasting side's numbering
  uint32_t item = 0;
  bool operator==(const FileListRequest&) const = default;
};
std::string EncodeFileListRequest(const FileListRequest& r);
std::optional<FileListRequest> DecodeFileListRequest(std::string_view payload);

struct FileList {
  uint64_t request = 0;
  std::vector<FileEntry> entries;  // the whole tree, the item itself first
  std::string error;               // non-empty: no entries
  bool operator==(const FileList&) const = default;
};
std::string EncodeFileList(const FileList& l);
std::optional<FileList> DecodeFileList(std::string_view payload);

// A fetch has the list request's fields.
using FileFetch = FileListRequest;

// ---- stream ----------------------------------------------------------------

std::string EncodeFileRecord(const FileEntry& e);  // length + header
std::string EncodeFileStreamEnd();

// Takes a stream in pieces of any size.
class FileStreamParser {
 public:
  struct Handler {
    virtual ~Handler() = default;
    virtual bool OnEntry(const FileEntry& e) = 0;          // false: stop with an error
    virtual bool OnContent(std::string_view bytes) = 0;    // part of the current file
    virtual bool OnEntryEnd() = 0;                         // the current entry is complete
    virtual void OnEnd() = 0;
  };
  explicit FileStreamParser(Handler* h) : h_(h) {}
  // False on a malformed stream or when the handler refused; error() says why.
  bool Feed(std::string_view bytes);
  bool ended() const { return ended_; }
  const std::string& error() const { return error_; }

 private:
  bool Fail(std::string why);
  Handler* h_;
  std::string buf_;  // a header being collected
  uint64_t content_left_ = 0;
  bool in_content_ = false;
  bool ended_ = false;
  std::string error_;
};

// ---- local files (POSIX) -----------------------------------------------------

// The entry for one local path (lstat; links are not followed); `name` goes
// into its path.
std::optional<FileEntry> StatLocalFile(const std::string& path, const std::string& name);
// The tree under `root` (the root itself first, path ""), folders before
// their contents, names sorted. False with `error` when it cannot be read or
// has more than `max` entries.
bool WalkLocalTree(const std::string& root, size_t max, std::vector<FileEntry>* out, std::string* error);

// ---- the pasting side's file system ---------------------------------------

// Asks for what the target needs from the peer (FileTransfer; any thread).
class FileRequester {
 public:
  virtual ~FileRequester() = default;
  virtual void RequestFileList(uint64_t announce, uint32_t item) = 0;
  virtual void RequestFileFetch(uint64_t announce, uint32_t item) = 0;
};

// Where pasted files appear (macOS: the File Provider extension). Called on
// the clipboard's thread.
class FileTarget {
 public:
  virtual ~FileTarget() = default;
  // Who to ask; null when the session ends.
  virtual void SetRequester(FileRequester* requester) = 0;
  // The peer copied these items (names, sizes, kinds): make them appear,
  // without content, and return a local path for each, in order, for the
  // clipboard. nullopt when files cannot be pasted here now (the clipboard
  // then gets their names only).
  virtual std::optional<std::vector<std::string>> Publish(uint64_t announce, const std::vector<FileEntry>& items) = 0;
  // What was published is no longer on offer.
  virtual void Withdraw(uint64_t announce) = 0;
  // Answers.
  virtual void OnList(uint64_t announce, uint32_t item, const std::vector<FileEntry>& entries,
                      const std::string& error) = 0;
  virtual void OnProgress(uint64_t announce, uint32_t item, const std::string& path, uint64_t bytes) = 0;
  // A file, link or folder of the item has arrived complete at `local_path`.
  virtual void OnFileReady(uint64_t announce, uint32_t item, const FileEntry& entry, const std::string& local_path) = 0;
  // The fetch of the item is over: everything arrived (error empty), or not.
  virtual void OnItemDone(uint64_t announce, uint32_t item, const std::string& error) = 0;
};

// ---- the engine ------------------------------------------------------------

// Both sides of file pasting for one session. Driven by ClipboardSync on its
// thread (Handle, Pump and the rest); only Post* are thread-safe.
class FileTransfer : public FileRequester {
 public:
  using SendFn = std::function<bool(uint32_t type, std::string_view payload)>;
  struct Options {
    std::shared_ptr<FileTarget> target;  // null: files are not pasted here
    // Where fetched items are written (pasting side): in a folder of this
    // session's own under it, removed when the session ends.
    std::string staging_dir;
    TransferPacer::Options pacer;
    std::function<uint64_t()> congestion_events;
    std::function<uint64_t()> clock;  // microseconds; empty = steady clock
    std::function<void(const std::string&)> log;
    // Posts a request to the clipboard's thread (ClipboardSync wires this).
    std::function<void(uint32_t kind, uint64_t announce, uint32_t item)> post;
  };

  FileTransfer(SendFn send, Options options);
  ~FileTransfer() override;

  bool CanPaste() const { return options_.target != nullptr; }

  // Copying side: this side's copy, announced as `announce`, is these local
  // paths (nothing before: an empty list).
  void SetOffered(uint64_t announce, std::vector<std::string> local_paths);
  // The entries to announce for `local_paths` (names, sizes, kinds), and in
  // *kept the paths they are for (those that can be read).
  static std::vector<FileEntry> Describe(const std::vector<std::string>& local_paths, std::vector<std::string>* kept);

  // Pasting side: the peer copied `items` as `announce`. The local paths for
  // the clipboard, or nullopt.
  std::optional<std::vector<std::string>> OnPeerAnnounce(uint64_t announce, const std::vector<FileEntry>& items);
  // The peer's copy is no longer on offer here.
  void Withdraw();

  void Handle(uint32_t type, const std::string& payload, uint64_t now);
  // Sends what the pacer allows; returns when to come back.
  uint64_t Pump(uint64_t now);
  void Stop();

  // FileRequester (any thread): posted to the clipboard's thread.
  void RequestFileList(uint64_t announce, uint32_t item) override;
  void RequestFileFetch(uint64_t announce, uint32_t item) override;
  // On the clipboard's thread, for what was posted.
  static constexpr uint32_t kPostList = 1, kPostFetch = 2;
  void OnPosted(uint32_t kind, uint64_t announce, uint32_t item);

 private:
  struct OutStream;
  struct InStream;

  // Copying side.
  void HandleListRequest(const std::string& payload);
  void HandleFetch(const std::string& payload);
  void StartNextStream();
  void HandleAck(const std::string& payload, uint64_t now);
  void DropOutgoing(const std::string& reason, bool tell_peer);
  // Pasting side.
  void HandleList(const std::string& payload);
  void HandleData(const std::string& payload);
  void HandleCancel(const std::string& payload);
  void FinishIncoming(uint64_t id, const std::string& error, bool tell_peer);
  std::string ItemDir(uint64_t announce, uint32_t item) const;
  void Log(const std::string& line);
  uint64_t Now() const;

  const SendFn send_;
  const Options options_;
  std::string staging_;  // this session's folder under options_.staging_dir
  uint64_t next_id_ = 0;

  // Copying side.
  uint64_t offered_announce_ = 0;
  std::vector<std::string> offered_;
  std::deque<FileFetch> fetches_;  // waiting for the outgoing slot
  std::unique_ptr<OutStream> out_;

  // Pasting side.
  uint64_t peer_announce_ = 0;
  std::vector<FileEntry> peer_items_;
  bool published_ = false;
  std::map<uint64_t, std::pair<uint64_t, uint32_t>> lists_;  // list request id -> (announce, item)
  std::map<uint64_t, std::unique_ptr<InStream>> ins_;       // fetch id -> stream
  std::set<uint32_t> fetching_;                              // items of peer_announce_ asked for
};

}  // namespace rdm
