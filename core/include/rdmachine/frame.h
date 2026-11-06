#pragma once
// Control-channel framing (docs/protocol.md, "Framing"): every message is a
// 4-byte big-endian length followed by that many bytes of UTF-8 JSON.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rdm {

inline constexpr size_t kFrameHeaderBytes = 4;
// Largest payload either side accepts. Control messages are small; anything
// bigger is a protocol error and the connection is closed.
inline constexpr size_t kMaxFramePayload = 64 * 1024;

// Returns nullopt when the payload is empty or larger than kMaxFramePayload.
std::optional<std::vector<uint8_t>> EncodeFrame(std::string_view payload);

// Incremental decoder for a byte stream (TLS records do not line up with
// frames). Feed whatever arrives; take complete payloads with Next().
class FrameDecoder {
 public:
  // Returns false (and stays failed) once a header announces a length of 0 or
  // more than kMaxFramePayload; the caller should close the connection.
  bool Feed(std::span<const uint8_t> bytes);
  std::optional<std::string> Next();
  bool failed() const { return failed_; }
  // Bytes received but not yet part of a complete frame.
  size_t buffered() const { return buf_.size(); }

 private:
  void Drain();

  std::vector<uint8_t> buf_;
  std::deque<std::string> ready_;
  bool failed_ = false;
};

}  // namespace rdm
