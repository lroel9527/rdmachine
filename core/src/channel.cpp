#include "rdmachine/channel.h"

#include <chrono>

namespace rdm {

bool MessageChannel::Send(const Message& m) {
  auto frame = EncodeFrame(SerializeMessage(m));
  return frame && conn_.WriteAll(frame->data(), frame->size());
}

MessageChannel::Received MessageChannel::Receive(int timeout_ms) {
  using std::chrono::steady_clock;
  const auto deadline = steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  Received r;
  for (;;) {
    if (decoder_.failed()) {
      r.status = Received::kProtocolError;
      r.error = "bad frame length";
      return r;
    }
    if (auto payload = decoder_.Next()) {
      ParseResult parsed = ParseMessage(*payload);
      if (!parsed.message) {
        r.status = Received::kProtocolError;
        r.error = parsed.error;
        return r;
      }
      r.status = Received::kMessage;
      r.message = std::move(parsed.message);
      return r;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - steady_clock::now()).count();
    if (left <= 0) {
      r.status = Received::kTimeout;
      return r;
    }
    uint8_t buf[4096];
    const int n = conn_.Read(buf, sizeof buf, static_cast<int>(left));
    if (n == TlsConnection::kTimeout) {
      r.status = Received::kTimeout;
      return r;
    }
    if (n <= 0) {
      r.status = Received::kClosed;
      return r;
    }
    decoder_.Feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
  }
}

}  // namespace rdm
