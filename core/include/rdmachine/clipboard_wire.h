#pragma once
// The clipboard's messages on the picture connection (docs/protocol.md,
// "剪贴板"): rdaccel application messages, reliable, ordered and encrypted.
//
// Version 2: nothing leaves a computer until the other side pastes it.
//   hello    (both ways, first)  {"version":2,"max_file_bytes":268435456}
//   announce (copying side)      something was copied: its formats and how
//                                many files, nothing else
//   request  (pasting side)      an application is pasting: send this format,
//                                or the files
//   offer    (copying side)      the answer: the format's size or the file list
//   data     (copying side)      the bytes of one item, in order
//   ack      (pasting side)      how many bytes of the offer are written
//   cancel   (either side)       an offer is given up
// A peer that never says hello (or says version 1) is an older version: the
// server gives it plain text as kClipboardTextMessageType, as before.
//
// Platform independent: encoding and checks only.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rdm {

// Plain UTF-8 text, the only clipboard message before version 1
// (rdaccel's RDA_MSG_CLIPBOARD_TEXT).
constexpr uint32_t kClipboardTextMessageType = 1;
// rdaccel application message types (RDA_MSG_USER_FIRST + 5 ... + 9).
constexpr uint32_t kClipboardHelloMessageType = 0x10005;
constexpr uint32_t kClipboardOfferMessageType = 0x10006;
constexpr uint32_t kClipboardDataMessageType = 0x10007;
constexpr uint32_t kClipboardAckMessageType = 0x10008;
constexpr uint32_t kClipboardCancelMessageType = 0x10009;
constexpr uint32_t kClipboardAnnounceMessageType = 0x1000A;
constexpr uint32_t kClipboardRequestMessageType = 0x1000B;

constexpr int kClipboardVersion = 2;

// Formats, as MIME types (the same names on every system).
inline constexpr std::string_view kMimeText = "text/plain";  // UTF-8
inline constexpr std::string_view kMimeRtf = "text/rtf";
inline constexpr std::string_view kMimeHtml = "text/html";   // UTF-8
inline constexpr std::string_view kMimePng = "image/png";

struct ClipboardHello {
  int version = kClipboardVersion;
  // The most file data this side takes in one copy; 0 = no files.
  uint64_t max_file_bytes = 0;

  bool operator==(const ClipboardHello&) const = default;
};

std::string EncodeClipboardHello(const ClipboardHello& h);
std::optional<ClipboardHello> DecodeClipboardHello(std::string_view payload);

// One file, directory or symbolic link of a copy. `path` is relative to the
// copy's root, '/'-separated, and every directory comes before what is in
// it; the top-level entries (no '/') are what was copied.
struct OfferEntry {
  enum class Kind { kFile, kDir, kLink };
  std::string path;
  Kind kind = Kind::kFile;
  uint64_t size = 0;    // files
  uint32_t mode = 0;    // permission bits (0777 at most); 0 = not given
  int64_t mtime = 0;    // seconds since 1970; 0 = not given
  std::string target;   // links

  bool operator==(const OfferEntry&) const = default;
};

struct OfferFormat {
  std::string mime;
  uint64_t size = 0;

  bool operator==(const OfferFormat&) const = default;
};

// announce: {"id":3,"formats":["text/plain","image/png"],"files":2}
struct ClipboardAnnounce {
  uint64_t id = 0;  // the copying side's own numbering
  std::vector<std::string> formats;
  uint64_t files = 0;  // copied files and folders (top level)

  bool operator==(const ClipboardAnnounce&) const = default;
};
std::string EncodeClipboardAnnounce(const ClipboardAnnounce& a);
std::optional<ClipboardAnnounce> DecodeClipboardAnnounce(std::string_view payload);

// request: {"announce":3,"id":1,"format":"text/plain"} or {"announce":3,"id":2,"files":true}
struct ClipboardRequest {
  uint64_t announce = 0;
  uint64_t id = 0;  // the pasting side's own numbering; the offer answering it says so
  std::string format;  // empty: the files
  bool operator==(const ClipboardRequest&) const = default;
};
std::string EncodeClipboardRequest(const ClipboardRequest& r);
std::optional<ClipboardRequest> DecodeClipboardRequest(std::string_view payload);

struct ClipboardOffer {
  uint64_t id = 0;  // the sender's own numbering
  uint64_t request = 0;  // the request it answers
  std::vector<OfferFormat> formats;
  std::vector<OfferEntry> entries;
  // Files that were copied but are not part of this offer (over the limit).
  uint64_t skipped_files = 0;
  uint64_t skipped_bytes = 0;
  std::string skipped_reason;  // "too_large", "too_many", "not_accepted", "gone"
  uint64_t skipped_limit = 0;  // the file limit that applied

  bool operator==(const ClipboardOffer&) const = default;

  // The data items, in the order their bytes are sent: every format, then
  // every file entry. Returns their sizes.
  std::vector<uint64_t> ItemSizes() const;
  uint64_t TotalBytes() const;
  uint64_t FileBytes() const;
  size_t FileCount() const;
};

std::string EncodeClipboardOffer(const ClipboardOffer& o);
// nullopt for anything malformed or unsafe (see ValidateOffer).
std::optional<ClipboardOffer> DecodeClipboardOffer(std::string_view payload);
// Every path a safe relative path (IsSafeRelativePath), no path twice, every
// parent a directory listed earlier, sizes and counts within reason.
bool ValidateOffer(const ClipboardOffer& o, std::string* why = nullptr);
// "a/b.txt" yes; "", "/a", "a/", "a//b", "a/./b", "../a", "a/..", invalid
// UTF-8, control characters (NUL included), names over 255 bytes, no.
bool IsSafeRelativePath(std::string_view path);

// data: id, item, offset (little endian: 8 + 4 + 8 bytes), then the bytes.
struct ClipboardDataHeader {
  uint64_t id = 0;
  uint32_t item = 0;
  uint64_t offset = 0;
};
constexpr size_t kClipboardDataHeaderSize = 20;
std::string EncodeClipboardData(const ClipboardDataHeader& h, std::string_view bytes);
// The header, with the bytes after it in *bytes (pointing into payload).
std::optional<ClipboardDataHeader> DecodeClipboardData(std::string_view payload, std::string_view* bytes);

// ack: id, bytes of the offer written so far, all items together (8 + 8).
struct ClipboardAck {
  uint64_t id = 0;
  uint64_t bytes = 0;

  bool operator==(const ClipboardAck&) const = default;
};
std::string EncodeClipboardAck(const ClipboardAck& a);
std::optional<ClipboardAck> DecodeClipboardAck(std::string_view payload);

// cancel: {"id":7,"from_sender":true,"reason":"superseded"}. from_sender
// tells which side's offer `id` numbers (each side numbers its own).
struct ClipboardCancel {
  uint64_t id = 0;
  bool from_sender = false;
  std::string reason;  // "superseded", "refused", "too_large", "error", ...

  bool operator==(const ClipboardCancel&) const = default;
};
std::string EncodeClipboardCancel(const ClipboardCancel& c);
std::optional<ClipboardCancel> DecodeClipboardCancel(std::string_view payload);

}  // namespace rdm
