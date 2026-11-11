#pragma once
// The clipboard's messages on the picture connection (docs/protocol.md,
// "剪贴板"): rdaccel application messages, reliable, ordered and encrypted.
//
// Version 3: nothing leaves a computer until the other side pastes it.
//   hello    (both ways, first)  {"version":3}
//   announce (copying side)      something was copied: its formats, nothing else
//   request  (pasting side)      an application is pasting: send this format
//   offer    (copying side)      the answer: the format's size
//   data     (copying side)      its bytes, in order
//   ack      (pasting side)      how many bytes are written
//   cancel   (either side)       an offer is given up
// A peer that never says hello (or says an earlier version) is an older
// version: the server gives it plain text as kClipboardTextMessageType, as
// before. Files are not part of the clipboard (a transfer window will carry
// them).
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
// rdaccel application message types (RDA_MSG_USER_FIRST + 5 ... + 11).
constexpr uint32_t kClipboardHelloMessageType = 0x10005;
constexpr uint32_t kClipboardOfferMessageType = 0x10006;
constexpr uint32_t kClipboardDataMessageType = 0x10007;
constexpr uint32_t kClipboardAckMessageType = 0x10008;
constexpr uint32_t kClipboardCancelMessageType = 0x10009;
constexpr uint32_t kClipboardAnnounceMessageType = 0x1000A;
constexpr uint32_t kClipboardRequestMessageType = 0x1000B;

constexpr int kClipboardVersion = 3;

// Formats, as MIME types (the same names on every system).
inline constexpr std::string_view kMimeText = "text/plain";  // UTF-8
inline constexpr std::string_view kMimeRtf = "text/rtf";
inline constexpr std::string_view kMimeHtml = "text/html";   // UTF-8
inline constexpr std::string_view kMimePng = "image/png";

struct ClipboardHello {
  int version = kClipboardVersion;

  bool operator==(const ClipboardHello&) const = default;
};

std::string EncodeClipboardHello(const ClipboardHello& h);
std::optional<ClipboardHello> DecodeClipboardHello(std::string_view payload);

// announce: {"id":3,"formats":["text/plain","image/png"]}
struct ClipboardAnnounce {
  uint64_t id = 0;  // the copying side's own numbering
  std::vector<std::string> formats;

  bool operator==(const ClipboardAnnounce&) const = default;
};
std::string EncodeClipboardAnnounce(const ClipboardAnnounce& a);
std::optional<ClipboardAnnounce> DecodeClipboardAnnounce(std::string_view payload);

// request: {"announce":3,"id":1,"format":"text/plain"}
struct ClipboardRequest {
  uint64_t announce = 0;
  uint64_t id = 0;  // the pasting side's own numbering; the offer answering it says so
  std::string format;

  bool operator==(const ClipboardRequest&) const = default;
};
std::string EncodeClipboardRequest(const ClipboardRequest& r);
std::optional<ClipboardRequest> DecodeClipboardRequest(std::string_view payload);

// offer: {"id":4,"request":1,"format":"text/plain","size":12}; or, when the
// request cannot be answered, {"id":4,"request":1,"skipped":"gone"} ("gone":
// copied over since; "too_large": over the limit).
struct ClipboardOffer {
  uint64_t id = 0;       // the sender's own numbering
  uint64_t request = 0;  // the request it answers
  std::string format;
  uint64_t size = 0;
  std::string skipped;

  bool operator==(const ClipboardOffer&) const = default;
};
std::string EncodeClipboardOffer(const ClipboardOffer& o);
std::optional<ClipboardOffer> DecodeClipboardOffer(std::string_view payload);

// data: id, offset (little endian: 8 + 8 bytes), then the bytes.
struct ClipboardDataHeader {
  uint64_t id = 0;
  uint64_t offset = 0;
};
constexpr size_t kClipboardDataHeaderSize = 16;
std::string EncodeClipboardData(const ClipboardDataHeader& h, std::string_view bytes);
// The header, with the bytes after it in *bytes (pointing into payload).
std::optional<ClipboardDataHeader> DecodeClipboardData(std::string_view payload, std::string_view* bytes);

// ack: id, bytes written so far (8 + 8).
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
