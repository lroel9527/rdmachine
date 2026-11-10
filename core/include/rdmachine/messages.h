#pragma once
// Control-protocol messages (docs/protocol.md, "Messages"). Each message is a
// JSON object whose "type" names it. Unknown fields are ignored so either
// side can add fields without breaking the other; an unknown "type" is an
// error the receiver answers with an `error` message.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "rdmachine/desktop.h"
#include "rdmachine/protocol.h"

namespace rdm {

// Client -> server, first message on a new connection.
struct ClientHello {
  int protocol = kProtocolVersion;
  std::string client_name;     // e.g. the client machine's name, for logs and the approval prompt
  std::string client_version;  // rdmachine version string
};

enum class AuthMethod { kPassword, kPairingCode };

// Server -> client, answer to ClientHello.
struct ServerHello {
  int protocol = kProtocolVersion;
  std::string server_name;
  std::string server_version;
  std::vector<AuthMethod> auth_methods;  // what the server accepts, in its order of preference
};

// Client -> server.
struct Auth {
  AuthMethod method = AuthMethod::kPassword;
  std::string username;  // kPassword
  std::string password;  // kPassword
  std::string code;      // kPairingCode
};

// Server -> client.
struct AuthResult {
  bool ok = false;
  std::string error;                    // when !ok: see docs/protocol.md "Error codes"
  std::optional<int64_t> retry_after_ms;  // when rate limited
};

enum class MediaTransport { kUdp, kTcp };

// Client -> server, after a successful AuthResult.
struct SessionRequest {
  int32_t display_index = 0;  // 0 = main display
  MediaTransport transport = MediaTransport::kUdp;
  bool view_only = false;
  // The desktop the client wants (docs/protocol.md, "请求桌面"): the server
  // builds a virtual display of this shape and shares that; display_index is
  // then not used. Empty: share the display as it is.
  std::optional<DesktopSpec> desktop;
  // Start in text mode (docs/protocol.md, "文字模式"): sharp text on a very
  // slow link. The client can change it later (kPictureModeMessageType).
  bool text_mode = false;
};

struct DisplayInfo {
  int32_t index = 0;
  std::string name;
  int32_t width = 0;   // pixels
  int32_t height = 0;  // pixels
  double scale = 1.0;  // pixels per point
};

// Server -> client: everything the client needs to create the rdaccel viewer.
struct SessionReady {
  MediaTransport transport = MediaTransport::kUdp;
  uint16_t port = 0;
  std::array<uint8_t, kSessionKeyBytes> key{};         // fresh per session
  std::array<uint8_t, kAuthTokenBytes> auth_token{};   // one use, short-lived
  std::vector<DisplayInfo> displays;
  // The desktop built for the request (the size may have been adjusted).
  // Empty: none was asked for or the server cannot build one; the picture
  // is the server's own display and the client scales it.
  std::optional<DesktopSpec> desktop;
};

struct Ping {
  int64_t id = 0;
};
struct Pong {
  int64_t id = 0;
};

// Either side, before closing.
struct Bye {
  std::string reason;
};

// Either side: the other side sent something it cannot act on.
struct Error {
  std::string code;
  std::string message;
};

using Message = std::variant<ClientHello, ServerHello, Auth, AuthResult, SessionRequest,
                             SessionReady, Ping, Pong, Bye, Error>;

// The "type" string a message is sent with.
const char* MessageType(const Message& m);

std::string SerializeMessage(const Message& m);

struct ParseResult {
  std::optional<Message> message;
  std::string error;  // set when message is empty
};
ParseResult ParseMessage(std::string_view json);

}  // namespace rdm
