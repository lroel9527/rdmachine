#include "rdmachine/server_session.h"

#include <algorithm>
#include <type_traits>

#include "rdmachine/random.h"

namespace rdm {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

AuthResult Refused(std::string code) {
  AuthResult r;
  r.error = std::move(code);
  return r;
}

}  // namespace

ServerSession::ServerSession(ServerDelegate& delegate, ServerGuards guards, std::string peer_ip,
                             std::function<Clock::time_point()> now)
    : delegate_(delegate), guards_(guards), peer_ip_(std::move(peer_ip)), now_(std::move(now)) {}

ServerSession::~ServerSession() {
  if (state_ == State::kInSession) delegate_.EndSession();
}

ServerSession::Output ServerSession::Fail(const char* code, std::string message) {
  if (state_ == State::kInSession) delegate_.EndSession();
  state_ = State::kClosed;
  Output out;
  out.send.push_back(Error{code, std::move(message)});
  out.close = true;
  return out;
}

ServerSession::Output ServerSession::HandleBadMessage(const std::string& error) {
  if (state_ == State::kClosed) return {{}, true};
  return Fail("bad_message", error);
}

ServerSession::Output ServerSession::Handle(const Message& in) {
  if (state_ == State::kClosed) return {{}, true};
  return std::visit(
      Overloaded{
          [&](const ClientHello& m) -> Output {
            if (state_ != State::kExpectHello) return Fail("unexpected", "hello already received");
            return OnHello(m);
          },
          [&](const Auth& m) -> Output {
            if (state_ != State::kExpectAuth) return Fail("unexpected", "auth not expected now");
            return OnAuth(m);
          },
          [&](const SessionRequest& m) -> Output {
            if (state_ == State::kInSession) {
              // A new request replaces the running session, e.g. TCP after
              // UDP did not get through (docs/protocol.md).
              delegate_.EndSession();
              state_ = State::kExpectSessionRequest;
            }
            if (state_ != State::kExpectSessionRequest) return Fail("unexpected", "session_request not expected now");
            return OnSessionRequest(m);
          },
          [&](const Ping& m) -> Output {
            if (state_ == State::kExpectHello) return Fail("unexpected", "hello must come first");
            return {{Pong{m.id}}, false};
          },
          [&](const Pong&) -> Output {
            if (state_ == State::kExpectHello) return Fail("unexpected", "hello must come first");
            return {};
          },
          [&](const Bye&) -> Output {
            if (state_ == State::kInSession) delegate_.EndSession();
            state_ = State::kClosed;
            return {{}, true};
          },
          [&](const Error&) -> Output {
            if (state_ == State::kInSession) delegate_.EndSession();
            state_ = State::kClosed;
            return {{}, true};
          },
          [&](const auto& m) -> Output {
            return Fail("unexpected", std::string(MessageType(Message(m))) + " is sent by servers only");
          },
      },
      in);
}

ServerSession::Output ServerSession::OnHello(const ClientHello& m) {
  ServerHello hello;
  hello.server_name = delegate_.ServerName();
  hello.server_version = VersionString();
  hello.auth_methods.push_back(AuthMethod::kPassword);
  if (guards_.pairing) hello.auth_methods.push_back(AuthMethod::kPairingCode);
  Output out;
  out.send.push_back(hello);
  if (m.protocol != kProtocolVersion) {
    state_ = State::kClosed;
    out.send.push_back(Error{"incompatible", "server speaks protocol " + std::to_string(kProtocolVersion)});
    out.close = true;
    return out;
  }
  state_ = State::kExpectAuth;
  return out;
}

ServerSession::Output ServerSession::OnAuth(const Auth& m) {
  const auto now = now_();
  if (auto wait = guards_.limiter->RetryAfter(peer_ip_, now)) {
    AuthResult r = Refused("rate_limited");
    r.retry_after_ms = wait->count();
    return {{r}, false};
  }
  std::string error;
  bool ok = false;
  if (m.method == AuthMethod::kPassword) {
    ok = !m.username.empty() && delegate_.CheckPassword(m.username, m.password, &error);
    if (!ok && error.empty()) error = "bad_credentials";
  } else if (!guards_.pairing) {
    return {{Refused("method_unsupported")}, false};
  } else {
    switch (guards_.pairing->Check(m.code, now)) {
      case PairingCodes::Result::kOk:
        ok = true;
        break;
      case PairingCodes::Result::kWrong:
        error = "bad_credentials";
        break;
      case PairingCodes::Result::kExpired:
      case PairingCodes::Result::kNone:
        error = "code_expired";
        break;
    }
  }
  if (!ok) {
    guards_.limiter->RecordFailure(peer_ip_, now);
    return {{Refused(error)}, false};
  }
  guards_.limiter->RecordSuccess(peer_ip_);
  origin_.peer_ip = peer_ip_;
  origin_.method = m.method;
  origin_.username = m.method == AuthMethod::kPassword ? m.username : std::string();
  state_ = State::kExpectSessionRequest;
  AuthResult r;
  r.ok = true;
  return {{r}, false};
}

ServerSession::Output ServerSession::OnSessionRequest(const SessionRequest& m) {
  if (m.display_index < 0) return Fail("no_display", "display_index must be >= 0");
  SessionSecrets secrets;
  if (!RandomBytes(secrets.key) || !RandomBytes(secrets.auth_token)) return Fail("internal", "random source failed");
  std::string error;
  std::optional<SessionGrant> grant = delegate_.StartSession(m, secrets, origin_, &error);
  if (!grant) {
    std::fill(secrets.key.begin(), secrets.key.end(), 0);
    return Fail(error.empty() ? "internal" : error.c_str(), "could not start the session");
  }
  state_ = State::kInSession;
  SessionReady ready;
  ready.transport = grant->transport;
  ready.port = grant->port;
  ready.key = secrets.key;
  ready.auth_token = secrets.auth_token;
  ready.displays = std::move(grant->displays);
  std::fill(secrets.key.begin(), secrets.key.end(), 0);
  return {{ready}, false};
}

}  // namespace rdm
