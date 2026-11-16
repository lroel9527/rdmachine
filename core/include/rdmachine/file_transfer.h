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
//   fetch        (pasting side)  {"announce":3,"id":2,"item":0}: send item 0, all of it;
//                                with "from":{"p":"sub/b.bin","o":4096,"s":9000,"t":1700000000}
//                                it goes on from there (below)
//   data         (copying side)  the item as one stream (below): id, offset, bytes
//   ack          (pasting side)  id, bytes written
//   cancel       (either side)   {"id":2,"from_sender":true,"reason":"error"}
// data, ack and cancel have the clipboard's encoding (clipboard_wire.h)
// under their own types; `id` is the fetch's.
//
// Compression: a fetch with "z":1 says the pasting side decodes Zstandard.
// The copying side may then send any piece of the stream as "zdata" instead
// of data: the same header (id, offset in the stream as it is uncompressed),
// the bytes one Zstandard frame that records its uncompressed size. Pieces
// that do not shrink (photos, videos, archives) go as plain data, so the
// cost on such files is a probe now and then. Offsets and acks count the
// uncompressed stream, so going on after a reconnect does not care.
//
// The stream of one item is a sequence of records, like a tar file:
//   u32 (little endian) length of the header, the header (JSON), then
//   `size` bytes of content for a regular file.
// Header: {"p":"sub/a.txt","k":"f","s":12,"m":420,"t":1700000000} with
// k = "f" (file), "d" (folder) or "l" (symbolic link, "l":"target"); "p" is
// relative to the item ("" = the item itself). The last record is {"end":true}.
// "x":{"name":"base64"} are the entry's extended attributes (Finder tags and
// the like; announcements and lists carry them too).
//
// Going on after a reconnect: the pasting side keeps what arrived and asks
// "from" the entry it was writing ("o": how much of it it has; "s", "t": the
// file as it was announced). If that file is unchanged, the stream sends
// every regular file before it with "o" equal to its size (no content: the
// pasting side has it), the file itself with "o" and the rest of its
// content, then everything after it as usual. Otherwise (or from an older
// peer) the stream starts over: records without "o" replace what is there.
//
// Copies are known by their announcement id, which the copying side makes
// unique (random): it answers fetches of its last copies, not just the one
// on its clipboard, so a paste that is under way goes on when either side
// copies something else, and after a reconnect.
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

struct ZSTD_DCtx_s;

namespace rdm {

// rdaccel application message types (RDA_MSG_USER_FIRST + 12 ... + 18).
constexpr uint32_t kFileListRequestMessageType = 0x1000C;
constexpr uint32_t kFileListMessageType = 0x1000D;
constexpr uint32_t kFileFetchMessageType = 0x1000E;
constexpr uint32_t kFileDataMessageType = 0x1000F;
constexpr uint32_t kFileAckMessageType = 0x10010;
constexpr uint32_t kFileCancelMessageType = 0x10011;
constexpr uint32_t kFileZDataMessageType = 0x10012;  // data, Zstandard

bool IsFileMessage(uint32_t type);

// Limits, both ways.
constexpr size_t kMaxFileDataPiece = 1 << 20;   // one data piece, uncompressed
constexpr size_t kMaxAnnouncedFiles = 1000;      // top-level items of one copy
constexpr size_t kMaxListedEntries = 100000;     // one item's tree
constexpr size_t kMaxFilePathBytes = 4096;
constexpr size_t kMaxXattrBytes = 16 * 1024;     // one entry's extended attributes

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
  // Extended attributes (name -> value), those worth copying and only up to
  // kMaxXattrBytes in all.
  std::map<std::string, std::string> xattrs;
  uint64_t offset = 0;  // in a stream: where the file's content starts

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

// Where a fetch goes on from (the stream comment above).
struct FileResumePoint {
  std::string path;
  uint64_t offset = 0;  // regular files
  uint64_t size = 0;
  int64_t mtime = 0;
  bool file = false;
  bool operator==(const FileResumePoint&) const = default;
};

struct FileFetch {
  uint64_t announce = 0;
  uint64_t id = 0;
  uint32_t item = 0;
  std::optional<FileResumePoint> from;
  bool zstd = false;  // the pasting side decodes zdata
  bool operator==(const FileFetch&) const = default;
};
std::string EncodeFileFetch(const FileFetch& f);
std::optional<FileFetch> DecodeFileFetch(std::string_view payload);

// A new copy's announcement id: random, never 0, below 2^53 (exact in any
// JSON reader).
uint64_t NewCopyId();

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
// The extended attributes of a path worth copying (not the system's own,
// such as sandbox and File Provider bookkeeping), up to kMaxXattrBytes.
std::map<std::string, std::string> ReadXattrs(const std::string& path);
void WriteXattrs(const std::string& path, const std::map<std::string, std::string>& xattrs);
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
  // `name`: the item's, as announced (where it is written).
  virtual void RequestFileFetch(uint64_t announce, uint32_t item, const std::string& name) = 0;
};

// Where pasted files appear (macOS: the File Provider extension). Called on
// the clipboard's thread.
class FileTarget {
 public:
  virtual ~FileTarget() = default;
  // Who to ask. A target may outlive a session and serve the next one: what
  // was being fetched when a connection dropped is asked for again from the
  // new requester (the same announcement and item), which goes on from where
  // the old one stopped.
  virtual void SetRequester(FileRequester* requester) = 0;
  // `requester` is going away (no-op if another one took its place).
  virtual void DropRequester(FileRequester* requester) = 0;
  // The peer copied these items (names, sizes, kinds): make them appear,
  // without content, and return a local path for each, in order, for the
  // clipboard. nullopt when files cannot be pasted here now (the clipboard
  // then gets their names only).
  virtual std::optional<std::vector<std::string>> Publish(uint64_t announce, const std::vector<FileEntry>& items) = 0;
  // What was published is no longer on the clipboard (the peer or this side
  // copied something else, or the session ended). What is being fetched from
  // it goes on; the target drops it when that is over.
  virtual void Withdraw(uint64_t announce) = 0;
  // The peer's files could not be published (Publish gave nullopt) and an
  // application pasted their names instead (macOS: RDMachine is turned off
  // in System Settings).
  virtual void OnNamesPasted() {}
  // Answers.
  virtual void OnList(uint64_t announce, uint32_t item, const std::vector<FileEntry>& entries,
                      const std::string& error) = 0;
  // A regular file of the item has begun to arrive at `local_path`; it grows
  // from there (OnProgress: how much of it is written), so what has arrived
  // can be read before the rest (macOS: Finder copies it as it comes).
  virtual void OnFileStarted(uint64_t announce, uint32_t item, const FileEntry& entry, const std::string& local_path) {
    (void)announce, (void)item, (void)entry, (void)local_path;
  }
  virtual void OnProgress(uint64_t announce, uint32_t item, const std::string& path, uint64_t bytes) = 0;
  // A file, link or folder of the item has arrived complete at `local_path`.
  virtual void OnFileReady(uint64_t announce, uint32_t item, const FileEntry& entry, const std::string& local_path) = 0;
  // The fetch of the item is over: everything arrived (error empty), or not.
  virtual void OnItemDone(uint64_t announce, uint32_t item, const std::string& error) = 0;
};

// ---- the engine ------------------------------------------------------------

// Removes what was fetched for a copy (`announce`) under a staging folder.
void RemoveStagedCopy(const std::string& staging_dir, uint64_t announce);

// Both sides of file pasting for one session. Driven by ClipboardSync on its
// thread (Handle, Pump and the rest); only Post* are thread-safe.
//
// Copying side: copies are remembered for the whole process (the last
// kKeptCopies), so a later session answers fetches of an earlier one's.
// Pasting side: fetched items are written under staging_dir/<announce>/<item>
// and stay there when the session ends; a fetch that was under way goes on
// in the next session (the target asks again).
class FileTransfer : public FileRequester {
 public:
  using SendFn = std::function<bool(uint32_t type, std::string_view payload)>;
  struct Options {
    std::shared_ptr<FileTarget> target;  // null: files are not pasted here
    // Where fetched items are written (pasting side). The target removes
    // what it no longer offers (RemoveStagedCopy).
    std::string staging_dir;
    TransferPacer::Options pacer;
    std::function<uint64_t()> congestion_events;
    std::function<uint64_t()> clock;  // microseconds; empty = steady clock
    std::function<void(const std::string&)> log;
    // The data of a stream goes through send_bulk when set: rdaccel's bulk
    // channel, below the picture. bulk_queued says how much of it waits in
    // the transport (-1: unknown, e.g. over TCP or with an older peer); when
    // known, the stream keeps that short instead of pacing itself.
    SendFn send_bulk;
    std::function<int64_t()> bulk_queued;
    // Posts a request to the clipboard's thread (ClipboardSync wires this).
    std::function<void(uint32_t kind, uint64_t announce, uint32_t item, const std::string& name)> post;
  };

  static constexpr size_t kKeptCopies = 32;

  FileTransfer(SendFn send, Options options);
  ~FileTransfer() override;

  bool CanPaste() const { return options_.target != nullptr; }

  // Copying side: this side's copy, announced as `announce` (NewCopyId), is
  // these local paths.
  void SetOffered(uint64_t announce, std::vector<std::string> local_paths);
  // The entries to announce for `local_paths` (names, sizes, kinds), and in
  // *kept the paths they are for (those that can be read).
  static std::vector<FileEntry> Describe(const std::vector<std::string>& local_paths, std::vector<std::string>* kept);

  // Pasting side: the peer copied `items` as `announce`. The local paths for
  // the clipboard, or nullopt. `unique_ids`: the peer makes announcement ids
  // unique (clipboard version 5); an older peer counts from 1 each session,
  // so its ids are made unique here (and its fetches do not go on after a
  // reconnect).
  std::optional<std::vector<std::string>> OnPeerAnnounce(uint64_t announce, const std::vector<FileEntry>& items,
                                                         bool unique_ids = true);
  // The peer's copy is no longer on the clipboard here (fetches under way go
  // on).
  void Withdraw();
  // An application pasted the names of files that could not be published.
  void NamesPasted();

  void Handle(uint32_t type, const std::string& payload, uint64_t now);
  // Sends what the pacer allows; returns when to come back.
  uint64_t Pump(uint64_t now);
  // The session ends: fetches under way are kept for the next one.
  void Stop();

  // FileRequester (any thread): posted to the clipboard's thread.
  void RequestFileList(uint64_t announce, uint32_t item) override;
  void RequestFileFetch(uint64_t announce, uint32_t item, const std::string& name) override;
  // On the clipboard's thread, for what was posted.
  static constexpr uint32_t kPostList = 1, kPostFetch = 2;
  void OnPosted(uint32_t kind, uint64_t announce, uint32_t item, const std::string& name);

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
  void HandleData(const std::string& payload, bool compressed);
  void HandleCancel(const std::string& payload);
  void FinishIncoming(uint64_t id, const std::string& error, bool tell_peer);
  std::string ItemDir(uint64_t key, uint32_t item) const;
  // The announcement id the peer knows a copy by (keys of older peers'
  // copies are made unique here).
  uint64_t WireAnnounce(uint64_t key) const;
  void Log(const std::string& line);
  uint64_t Now() const;
  bool SendData(uint32_t type, std::string_view payload);
  static std::string SentLine(const OutStream& o);
  // Pump with the transport's bulk queue known: fill it up to a target.
  uint64_t PumpBulk(uint64_t now, int64_t queued);

  const SendFn send_;
  const Options options_;
  uint64_t next_id_ = 0;

  // Copying side.
  std::deque<FileFetch> fetches_;  // waiting for the outgoing slot
  std::unique_ptr<OutStream> out_;

  // Pasting side.
  uint64_t peer_key_ = 0;  // what is on the clipboard (0: nothing)
  bool published_ = false;
  bool names_only_ = false;  // the peer's files are on the clipboard as names
  uint64_t salt_ = 0;
  std::map<uint64_t, uint64_t> salted_;                       // key -> an older peer's announcement id
  std::map<uint64_t, std::pair<uint64_t, uint32_t>> lists_;  // list request id -> (key, item)
  std::map<uint64_t, std::unique_ptr<InStream>> ins_;       // fetch id -> stream
  std::set<std::pair<uint64_t, uint32_t>> fetching_;         // (key, item) asked for
  ZSTD_DCtx_s* zstd_in_ = nullptr;                             // zdata
  std::string zbuf_in_;
};

}  // namespace rdm
