#pragma once
// Framed control messages over a TLS connection.
#include <string>

#include "rdmachine/frame.h"
#include "rdmachine/messages.h"
#include "rdmachine/tls.h"

namespace rdm {

class MessageChannel {
 public:
  explicit MessageChannel(TlsConnection& conn) : conn_(conn) {}

  bool Send(const Message& m);

  struct Received {
    enum Status { kMessage, kTimeout, kClosed, kProtocolError } status = kClosed;
    std::optional<Message> message;
    std::string error;  // kProtocolError: what was wrong
  };
  // Waits up to timeout_ms for one complete message.
  Received Receive(int timeout_ms);

 private:
  TlsConnection& conn_;
  FrameDecoder decoder_;
};

}  // namespace rdm
