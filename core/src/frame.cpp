#include "rdmachine/frame.h"

namespace rdm {

std::optional<std::vector<uint8_t>> EncodeFrame(std::string_view payload) {
  if (payload.empty() || payload.size() > kMaxFramePayload) return std::nullopt;
  const uint32_t n = static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> out;
  out.reserve(kFrameHeaderBytes + payload.size());
  out.push_back(uint8_t(n >> 24));
  out.push_back(uint8_t(n >> 16));
  out.push_back(uint8_t(n >> 8));
  out.push_back(uint8_t(n));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool FrameDecoder::Feed(std::span<const uint8_t> bytes) {
  if (failed_) return false;
  buf_.insert(buf_.end(), bytes.begin(), bytes.end());
  Drain();
  return !failed_;
}

void FrameDecoder::Drain() {
  size_t pos = 0;
  while (buf_.size() - pos >= kFrameHeaderBytes) {
    const uint32_t n = (uint32_t{buf_[pos]} << 24) | (uint32_t{buf_[pos + 1]} << 16) |
                       (uint32_t{buf_[pos + 2]} << 8) | uint32_t{buf_[pos + 3]};
    if (n == 0 || n > kMaxFramePayload) {
      failed_ = true;
      buf_.clear();
      return;
    }
    if (buf_.size() - pos - kFrameHeaderBytes < n) break;
    const auto* p = reinterpret_cast<const char*>(buf_.data() + pos + kFrameHeaderBytes);
    ready_.emplace_back(p, n);
    pos += kFrameHeaderBytes + n;
  }
  buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
}

std::optional<std::string> FrameDecoder::Next() {
  if (ready_.empty()) return std::nullopt;
  std::string s = std::move(ready_.front());
  ready_.pop_front();
  return s;
}

}  // namespace rdm
