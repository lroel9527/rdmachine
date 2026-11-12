#include <gtest/gtest.h>

#include "fake_delegate.h"
#include "rdmachine/server_session.h"

using namespace rdm;
using namespace std::chrono_literals;
using rdm::testing::FakeDelegate;

namespace {

class ServerSessionTest : public ::testing::Test {
 protected:
  ServerSession Make(bool pairing = true) {
    return ServerSession(delegate_, ServerGuards{&limiter_, pairing ? &pairing_ : nullptr}, "10.0.0.2",
                         [this] { return now_; });
  }
  static Auth Password(std::string user, std::string pass) {
    Auth a;
    a.username = std::move(user);
    a.password = std::move(pass);
    return a;
  }
  static Auth Code(std::string code) {
    Auth a;
    a.method = AuthMethod::kPairingCode;
    a.code = std::move(code);
    return a;
  }
  static const AuthResult& AsAuthResult(const ServerSession::Output& out) {
    EXPECT_EQ(out.send.size(), 1u);
    return std::get<AuthResult>(out.send.at(0));
  }
  static std::string ErrorCode(const ServerSession::Output& out) {
    EXPECT_TRUE(out.close);
    EXPECT_FALSE(out.send.empty());
    return std::get<Error>(out.send.back()).code;
  }

  FakeDelegate delegate_;
  FailureLimiter limiter_;
  PairingCodes pairing_;
  Clock::time_point now_ = Clock::time_point() + 1000h;
};

}  // namespace

TEST_F(ServerSessionTest, HappyPathWithPassword) {
  auto s = Make();
  auto out = s.Handle(ClientHello{0, "laptop", "0.1"});
  ASSERT_EQ(out.send.size(), 1u);
  const auto& hello = std::get<ServerHello>(out.send[0]);
  EXPECT_EQ(hello.server_name, "test-mac");
  EXPECT_EQ(hello.auth_methods, (std::vector{AuthMethod::kPassword, AuthMethod::kPairingCode}));
  EXPECT_FALSE(out.close);

  EXPECT_TRUE(AsAuthResult(s.Handle(Password("alice", "secret"))).ok);

  SessionRequest req;
  req.view_only = true;
  out = s.Handle(req);
  ASSERT_EQ(out.send.size(), 1u);
  const auto& ready = std::get<SessionReady>(out.send[0]);
  EXPECT_EQ(ready.port, 4100);
  EXPECT_EQ(ready.key, delegate_.last_secrets.key);
  EXPECT_EQ(ready.auth_token, delegate_.last_secrets.auth_token);
  EXPECT_NE(ready.key, (std::array<uint8_t, 32>{}));
  EXPECT_NE(ready.key, ready.auth_token);
  EXPECT_TRUE(delegate_.last_request.view_only);
  EXPECT_EQ(s.state(), ServerSession::State::kInSession);
  EXPECT_EQ(delegate_.last_origin.method, AuthMethod::kPassword);
  EXPECT_EQ(delegate_.last_origin.username, "alice");
  EXPECT_EQ(delegate_.last_origin.peer_ip, "10.0.0.2");

  out = s.Handle(Ping{7});
  EXPECT_EQ(std::get<Pong>(out.send.at(0)).id, 7);

  out = s.Handle(Bye{"user_closed"});
  EXPECT_TRUE(out.close);
  EXPECT_EQ(delegate_.ends, 1);
}

TEST_F(ServerSessionTest, FreshSecretsEachSession) {
  std::array<uint8_t, 32> first{};
  for (int i = 0; i < 2; ++i) {
    auto s = Make();
    s.Handle(ClientHello{});
    s.Handle(Password("alice", "secret"));
    s.Handle(SessionRequest{});
    if (i == 0) first = delegate_.last_secrets.key;
  }
  EXPECT_NE(first, delegate_.last_secrets.key);
  EXPECT_EQ(delegate_.ends, 2);  // destructor ends a started session
}

TEST_F(ServerSessionTest, IncompatibleProtocol) {
  auto s = Make();
  auto out = s.Handle(ClientHello{1, "x", "y"});
  ASSERT_EQ(out.send.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<ServerHello>(out.send[0]));
  EXPECT_EQ(ErrorCode(out), "incompatible");
}

TEST_F(ServerSessionTest, OutOfOrderMessages) {
  {
    auto s = Make();
    EXPECT_EQ(ErrorCode(s.Handle(Password("alice", "secret"))), "unexpected");
  }
  {
    auto s = Make();
    s.Handle(ClientHello{});
    EXPECT_EQ(ErrorCode(s.Handle(SessionRequest{})), "unexpected");
    EXPECT_EQ(delegate_.starts, 0);
  }
  {
    auto s = Make();
    s.Handle(ClientHello{});
    EXPECT_EQ(ErrorCode(s.Handle(ClientHello{})), "unexpected");
  }
  {
    auto s = Make();
    EXPECT_EQ(ErrorCode(s.Handle(SessionReady{})), "unexpected");
  }
}

TEST_F(ServerSessionTest, BadMessage) {
  auto s = Make();
  EXPECT_EQ(ErrorCode(s.HandleBadMessage("missing type")), "bad_message");
  EXPECT_EQ(s.state(), ServerSession::State::kClosed);
}

TEST_F(ServerSessionTest, WrongPasswordThenRight) {
  auto s = Make();
  s.Handle(ClientHello{});
  auto r = AsAuthResult(s.Handle(Password("alice", "nope")));
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.error, "bad_credentials");
  EXPECT_EQ(AsAuthResult(s.Handle(Password("bob", "x"))).error, "not_console_user");
  EXPECT_EQ(AsAuthResult(s.Handle(Password("", ""))).error, "bad_credentials");
  EXPECT_TRUE(AsAuthResult(s.Handle(Password("alice", "secret"))).ok);
}

TEST_F(ServerSessionTest, RateLimited) {
  auto s = Make();
  s.Handle(ClientHello{});
  for (int i = 0; i < 5; ++i) s.Handle(Password("alice", "nope"));
  auto r = AsAuthResult(s.Handle(Password("alice", "secret")));
  EXPECT_EQ(r.error, "rate_limited");
  ASSERT_TRUE(r.retry_after_ms);
  EXPECT_EQ(*r.retry_after_ms, 60000);
  now_ += 61s;
  EXPECT_TRUE(AsAuthResult(s.Handle(Password("alice", "secret"))).ok);
}

TEST_F(ServerSessionTest, PairingCode) {
  auto s = Make();
  s.Handle(ClientHello{});
  EXPECT_EQ(AsAuthResult(s.Handle(Code("12345678"))).error, "code_expired");  // none issued
  std::string code = pairing_.Issue(now_);
  EXPECT_TRUE(AsAuthResult(s.Handle(Code(code))).ok);
  s.Handle(SessionRequest{});
  EXPECT_EQ(delegate_.last_origin.method, AuthMethod::kPairingCode);
  EXPECT_EQ(delegate_.last_origin.username, "");
}

TEST_F(ServerSessionTest, PairingCodeOff) {
  auto s = Make(false);
  auto out = s.Handle(ClientHello{});
  EXPECT_EQ(std::get<ServerHello>(out.send[0]).auth_methods, std::vector{AuthMethod::kPassword});
  EXPECT_EQ(AsAuthResult(s.Handle(Code("12345678"))).error, "method_unsupported");
}

TEST_F(ServerSessionTest, StartSessionErrors) {
  auto s = Make();
  s.Handle(ClientHello{});
  s.Handle(Password("alice", "secret"));
  SessionRequest req;
  req.display_index = 3;
  EXPECT_EQ(ErrorCode(s.Handle(req)), "no_display");
  EXPECT_EQ(delegate_.ends, 0);
}

TEST_F(ServerSessionTest, SecondRequestReplacesSession) {
  auto s = Make();
  s.Handle(ClientHello{});
  s.Handle(Password("alice", "secret"));
  s.Handle(SessionRequest{});
  const auto first_key = delegate_.last_secrets.key;
  SessionRequest tcp;
  tcp.transport = MediaTransport::kTcp;
  auto out = s.Handle(tcp);
  const auto& ready = std::get<SessionReady>(out.send.at(0));
  EXPECT_EQ(ready.transport, MediaTransport::kTcp);
  EXPECT_NE(ready.key, first_key);
  EXPECT_EQ(delegate_.starts, 2);
  EXPECT_EQ(delegate_.ends, 1);
  EXPECT_EQ(s.state(), ServerSession::State::kInSession);
}

namespace {

// Logs in and starts a session; returns the session in kInSession.
void StartSession(ServerSession& s) {
  ClientHello h;
  s.Handle(h);
  Auth a;
  a.username = "alice";
  a.password = "secret";
  s.Handle(a);
  auto out = s.Handle(SessionRequest{});
  ASSERT_EQ(out.send.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<SessionReady>(out.send[0]));
  ASSERT_EQ(s.state(), ServerSession::State::kInSession);
}

}  // namespace

TEST_F(ServerSessionTest, TickKeepsASessionTheDelegateDoesNotMove) {
  auto s = Make();
  StartSession(s);
  auto out = s.Tick();
  EXPECT_TRUE(out.send.empty());
  EXPECT_FALSE(out.close);
  EXPECT_EQ(s.state(), ServerSession::State::kInSession);
}

TEST_F(ServerSessionTest, AMovedSessionSaysSoThenStartsAgainWithNewSecrets) {
  auto s = Make();
  StartSession(s);
  const auto first_key = delegate_.last_secrets.key;
  delegate_.check = {SessionCheck::Action::kMove, "user_logged_in"};
  auto out = s.Tick();
  ASSERT_EQ(out.send.size(), 1u);
  EXPECT_EQ(std::get<SessionMoving>(out.send[0]).reason, "user_logged_in");
  EXPECT_EQ(s.state(), ServerSession::State::kMoving);
  EXPECT_EQ(delegate_.ends, 0);  // the client hears of the move before the picture stops
  out = s.Tick();  // the next tick ends the old session and starts it again
  ASSERT_EQ(out.send.size(), 1u);
  const auto& ready = std::get<SessionReady>(out.send[0]);
  EXPECT_NE(ready.key, first_key);
  EXPECT_FALSE(out.close);
  EXPECT_EQ(delegate_.ends, 1);
  EXPECT_EQ(delegate_.starts, 2);
  EXPECT_EQ(s.state(), ServerSession::State::kInSession);
}

TEST_F(ServerSessionTest, AMoveRetriesEverySecondUntilTheNewSessionStarts) {
  auto s = Make();
  StartSession(s);
  delegate_.check = {SessionCheck::Action::kMove, "user_logged_in"};
  delegate_.refuse_starts = 2;  // the new session's agent is not there yet
  auto out = s.Tick();
  ASSERT_EQ(out.send.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<SessionMoving>(out.send[0]));
  EXPECT_EQ(s.state(), ServerSession::State::kMoving);
  EXPECT_TRUE(s.Tick().send.empty());  // refused
  EXPECT_TRUE(s.Tick().send.empty());  // not before a second has passed
  now_ += 1s;
  EXPECT_TRUE(s.Tick().send.empty());  // refused again
  EXPECT_EQ(s.state(), ServerSession::State::kMoving);
  now_ += 1s;
  out = s.Tick();
  ASSERT_EQ(out.send.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<SessionReady>(out.send[0]));
  EXPECT_EQ(s.state(), ServerSession::State::kInSession);
}

TEST_F(ServerSessionTest, AMoveThatNeverSucceedsFailsAfterTheTimeout) {
  auto s = Make();
  StartSession(s);
  delegate_.check = {SessionCheck::Action::kMove, "user_logged_in"};
  delegate_.refuse_starts = 1000;
  s.Tick();
  EXPECT_EQ(s.state(), ServerSession::State::kMoving);
  ServerSession::Output out;
  for (int i = 0; i < 70 && s.state() == ServerSession::State::kMoving; ++i) {
    now_ += 1s;
    out = s.Tick();
  }
  EXPECT_EQ(s.state(), ServerSession::State::kClosed);
  EXPECT_EQ(ErrorCode(out), "unavailable");
  EXPECT_EQ(delegate_.ends, 1);  // nothing was running to end again
}

TEST_F(ServerSessionTest, ARequestWhileMovingIsWhatStartsAgain) {
  auto s = Make();
  StartSession(s);
  delegate_.check = {SessionCheck::Action::kMove, "user_logged_in"};
  s.Tick();
  ASSERT_EQ(s.state(), ServerSession::State::kMoving);
  SessionRequest tcp;
  tcp.transport = MediaTransport::kTcp;
  EXPECT_TRUE(s.Handle(tcp).send.empty());
  auto out = s.Tick();
  ASSERT_EQ(out.send.size(), 1u);
  EXPECT_EQ(std::get<SessionReady>(out.send[0]).transport, MediaTransport::kTcp);
}

TEST_F(ServerSessionTest, ByeWhileMovingEndsNothingTwice) {
  auto s = Make();
  StartSession(s);
  delegate_.check = {SessionCheck::Action::kMove, "user_logged_in"};
  delegate_.refuse_starts = 5;
  s.Tick();
  auto out = s.Handle(Bye{"user_closed"});
  EXPECT_TRUE(out.close);
  EXPECT_EQ(delegate_.ends, 1);
}

TEST_F(ServerSessionTest, TheDelegateCanEndASession) {
  auto s = Make();
  StartSession(s);
  delegate_.check = {SessionCheck::Action::kEnd, "console_changed"};
  auto out = s.Tick();
  ASSERT_EQ(out.send.size(), 1u);
  EXPECT_EQ(std::get<Bye>(out.send[0]).reason, "console_changed");
  EXPECT_TRUE(out.close);
  EXPECT_EQ(s.state(), ServerSession::State::kClosed);
  EXPECT_EQ(delegate_.ends, 0);  // the reason goes out first
}

TEST_F(ServerSessionTest, AnEndedSessionStopsWhenTheConnectionCloses) {
  {
    auto s = Make();
    StartSession(s);
    delegate_.check = {SessionCheck::Action::kEnd, "console_changed"};
    s.Tick();
  }
  EXPECT_EQ(delegate_.ends, 1);
}

