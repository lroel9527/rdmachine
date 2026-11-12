#pragma once
// The server's side of one control connection, as a state machine with no
// I/O: feed it each received message, send what it returns. ControlServer
// drives it over TLS; tests drive it directly.
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rdmachine/auth_guards.h"
#include "rdmachine/messages.h"

namespace rdm {

struct SessionSecrets {
  std::array<uint8_t, kSessionKeyBytes> key{};
  std::array<uint8_t, kAuthTokenBytes> auth_token{};
};

struct SessionGrant {
  MediaTransport transport = MediaTransport::kUdp;
  uint16_t port = 0;
  std::vector<DisplayInfo> displays;
  std::optional<DesktopSpec> desktop;  // built for request.desktop
  bool login_window = false;           // the picture is the login window
};

// Who a session is for, as the control connection saw it (for status
// displays and logs; not used for any security decision).
struct SessionOrigin {
  std::string peer_ip;
  AuthMethod method = AuthMethod::kPassword;
  std::string username;  // empty for pairing-code logins
  // The account whose desktop this login is for: the username, or for a
  // pairing code whoever owned the console when it was used
  // (ServerDelegate::PairingAccount; empty: unknown or the login window).
  std::string account;
};

// What becomes of a running session (ServerDelegate::CheckSession).
struct SessionCheck {
  enum class Action {
    kKeep,  // carry on
    kMove,  // the picture is now elsewhere (another graphical session): end
            // this one, tell the client (session_moving) and start it again
    kEnd,   // end the session and the connection (bye with `reason`)
  };
  Action action = Action::kKeep;
  std::string reason;
};

// What the platform provides (on macOS: PAM and the rdaccel host).
class ServerDelegate {
 public:
  virtual ~ServerDelegate() = default;

  virtual std::string ServerName() = 0;
  // `error` is an auth_result code: bad_credentials, not_console_user, ...
  virtual bool CheckPassword(const std::string& username, const std::string& password, std::string* error) = 0;
  // Starts the rdaccel host for this request with these secrets (the delegate
  // registers the token for on_auth). On failure sets an error code:
  // no_display, capture_denied, busy, internal.
  virtual std::optional<SessionGrant> StartSession(const SessionRequest& request, const SessionSecrets& secrets,
                                                   const SessionOrigin& origin, std::string* error) = 0;
  // The control connection of a started session ended.
  virtual void EndSession() = 0;
  // The account a pairing code just used logs in to (SessionOrigin::account).
  virtual std::string PairingAccount() { return {}; }
  // Asked a few times a second while a session runs or is moving (see
  // SessionCheck). The default keeps every session where it is.
  virtual SessionCheck CheckSession(const SessionOrigin& origin) {
    (void)origin;
    return {};
  }
};

struct ServerGuards {
  FailureLimiter* limiter = nullptr;      // required
  PairingCodes* pairing = nullptr;        // null: pairing codes not offered
};

class ServerSession {
 public:
  ServerSession(ServerDelegate& delegate, ServerGuards guards, std::string peer_ip,
                std::function<Clock::time_point()> now = [] { return Clock::now(); });
  ~ServerSession();  // ends a started session

  struct Output {
    std::vector<Message> send;
    bool close = false;  // close the connection after sending
  };
  Output Handle(const Message& in);
  // A received frame that did not parse.
  Output HandleBadMessage(const std::string& error);
  // Called regularly (every few hundred ms) by the connection loop: moves or
  // ends a session when the delegate says so (CheckSession), and retries the
  // start of a moving session until it succeeds or kMoveTimeout passes.
  Output Tick();

  // kMoving: the session ended for a move and starts again elsewhere.
  enum class State { kExpectHello, kExpectAuth, kExpectSessionRequest, kInSession, kMoving, kClosed };
  static constexpr auto kMoveTimeout = std::chrono::seconds(60);
  State state() const { return state_; }

 private:
  Output Fail(const char* code, std::string message);
  Output OnHello(const ClientHello& m);
  Output OnAuth(const Auth& m);
  Output OnSessionRequest(const SessionRequest& m);
  // Starts the session for `m`; nullopt with *error on failure.
  std::optional<SessionReady> TryStart(const SessionRequest& m, std::string* error);
  void EndStarted();  // EndSession if a session was started and not ended yet

  ServerDelegate& delegate_;
  ServerGuards guards_;
  std::string peer_ip_;
  SessionOrigin origin_;  // filled at login
  std::function<Clock::time_point()> now_;
  State state_ = State::kExpectHello;
  bool started_ = false;          // the delegate runs a session for this connection
  SessionRequest last_request_;  // what a moved session starts again with
  Clock::time_point move_deadline_{}, next_try_{};
};

}  // namespace rdm
