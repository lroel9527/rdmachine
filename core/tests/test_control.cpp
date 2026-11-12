// TLS and the control protocol end to end over loopback.
#include <gtest/gtest.h>

#include <thread>

#include "fake_delegate.h"
#include "rdmachine/control_client.h"
#include "rdmachine/control_server.h"
#include "rdmachine/tls.h"

using namespace rdm;
using rdm::testing::FakeDelegate;

namespace {

const TlsIdentity& Identity() {
  static const TlsIdentity id = [] {
    std::string error;
    auto i = GenerateTlsIdentity("test-mac", &error);
    EXPECT_TRUE(i) << error;
    return *i;
  }();
  return id;
}

class ControlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ControlServerOptions opts;
    opts.bind_address = "127.0.0.1";
    opts.port = 0;
    opts.handshake_timeout_ms = 2000;
    server_ = std::make_unique<ControlServer>(delegate_, Identity(), opts);
    std::string error;
    ASSERT_TRUE(server_->Start(&error)) << error;
    ASSERT_NE(server_->port(), 0);
  }

  ClientOptions Options() {
    ClientOptions o;
    o.host = "127.0.0.1";
    o.port = server_->port();
    o.client_name = "laptop";
    o.auth.username = "alice";
    o.auth.password = "secret";
    o.trust_server = [](const std::string&) { return true; };
    o.timeout_ms = 5000;
    return o;
  }

  FakeDelegate delegate_;
  std::unique_ptr<ControlServer> server_;
};

}  // namespace

TEST(Tls, IdentityAndFingerprint) {
  auto fp = CertificateFingerprint(Identity().cert_pem);
  ASSERT_TRUE(fp);
  EXPECT_EQ(fp->rfind("sha256:", 0), 0u);
  EXPECT_EQ(fp->size(), 7u + 64u);
  EXPECT_EQ(CertificateFingerprint(Identity().cert_pem), fp);  // stable
  EXPECT_NE(Identity().key_pem.find("PRIVATE KEY"), std::string::npos);

  std::string error;
  auto other = GenerateTlsIdentity("x,CN=evil", &error);
  ASSERT_TRUE(other) << error;
  EXPECT_NE(CertificateFingerprint(other->cert_pem), fp);
  EXPECT_FALSE(CertificateFingerprint("not a certificate"));

  std::string formatted = FormatFingerprint(*fp);
  EXPECT_EQ(formatted.size(), 64u + 15u);
  EXPECT_EQ(formatted[4], ' ');
}

TEST_F(ControlTest, PasswordLoginAndSession) {
  auto r = ConnectAndStartSession(Options());
  ASSERT_EQ(r.error, "") << r.detail;
  EXPECT_EQ(r.fingerprint, CertificateFingerprint(Identity().cert_pem));
  EXPECT_EQ(r.server.server_name, "test-mac");
  EXPECT_EQ(r.session.port, 4100);
  EXPECT_EQ(r.session.key, delegate_.last_secrets.key);
  EXPECT_EQ(r.session.auth_token, delegate_.last_secrets.auth_token);
  ASSERT_EQ(r.session.displays.size(), 1u);
  EXPECT_EQ(r.session.displays[0].width, 2880);

  // The connection stays open for the session; ping it.
  ASSERT_TRUE(r.connection->channel().Send(Ping{42}));
  auto pong = r.connection->channel().Receive(5000);
  ASSERT_EQ(pong.status, MessageChannel::Received::kMessage);
  EXPECT_EQ(std::get<Pong>(*pong.message).id, 42);

  r.connection->Close("user_closed");
  for (int i = 0; i < 100 && server_->active_connections() > 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(server_->active_connections(), 0);
  std::lock_guard lock(delegate_.mu);
  EXPECT_EQ(delegate_.ends, 1);
}

TEST_F(ControlTest, UntrustedServerSendsNothing) {
  auto o = Options();
  std::string seen;
  o.trust_server = [&](const std::string& fp) {
    seen = fp;
    return false;
  };
  auto r = ConnectAndStartSession(o);
  EXPECT_EQ(r.error, "untrusted_server");
  EXPECT_EQ(seen, CertificateFingerprint(Identity().cert_pem));
  EXPECT_FALSE(r.connection);
  EXPECT_EQ(delegate_.starts, 0);
}

TEST_F(ControlTest, BadCredentials) {
  auto o = Options();
  o.auth.password = "wrong";
  auto r = ConnectAndStartSession(o);
  EXPECT_EQ(r.error, "bad_credentials");
  EXPECT_FALSE(r.connection);
}

TEST_F(ControlTest, RateLimitedAcrossConnections) {
  auto o = Options();
  o.auth.password = "wrong";
  for (int i = 0; i < 5; ++i) EXPECT_EQ(ConnectAndStartSession(o).error, "bad_credentials");
  auto r = ConnectAndStartSession(Options());
  EXPECT_EQ(r.error, "rate_limited");
  ASSERT_TRUE(r.retry_after_ms);
  EXPECT_GT(*r.retry_after_ms, 0);
}

TEST_F(ControlTest, PairingCode) {
  std::string code = server_->IssuePairingCode();
  ASSERT_EQ(code.size(), 8u);
  auto o = Options();
  o.auth = Auth{};
  o.auth.method = AuthMethod::kPairingCode;
  o.auth.code = code;
  auto r = ConnectAndStartSession(o);
  ASSERT_EQ(r.error, "") << r.detail;
  r.connection.reset();
  // Used once.
  EXPECT_EQ(ConnectAndStartSession(o).error, "code_expired");
}

TEST_F(ControlTest, ASecondClientTakesTheSessionOver) {
  auto first = ConnectAndStartSession(Options());
  ASSERT_EQ(first.error, "") << first.detail;
  auto second = ConnectAndStartSession(Options());
  EXPECT_EQ(second.error, "") << second.detail;
  // The first one was told why before its connection closed.
  std::string reason;
  for (int i = 0; i < 20 && reason.empty(); ++i) {
    auto r = first.connection->channel().Receive(250);
    if (r.status == MessageChannel::Received::kClosed) break;
    if (r.status == MessageChannel::Received::kMessage) {
      if (auto* bye = std::get_if<Bye>(&*r.message)) reason = bye->reason;
    }
  }
  EXPECT_EQ(reason, "session_replaced");
}

TEST_F(ControlTest, GarbageFrameGetsBadMessage) {
  std::string error;
  auto conn = TlsConnect("127.0.0.1", server_->port(), 5000, &error);
  ASSERT_TRUE(conn) << error;
  const std::string json = R"({"type":"hello","protocol":"zero"})";
  auto frame = EncodeFrame(json);
  ASSERT_TRUE(conn->WriteAll(frame->data(), frame->size()));
  MessageChannel ch(*conn);
  auto r = ch.Receive(5000);
  ASSERT_EQ(r.status, MessageChannel::Received::kMessage);
  EXPECT_EQ(std::get<Error>(*r.message).code, "bad_message");
  EXPECT_EQ(ch.Receive(5000).status, MessageChannel::Received::kClosed);
}

TEST_F(ControlTest, OversizedFrameClosesConnection) {
  std::string error;
  auto conn = TlsConnect("127.0.0.1", server_->port(), 5000, &error);
  ASSERT_TRUE(conn) << error;
  const uint8_t header[4] = {0x7f, 0xff, 0xff, 0xff};
  ASSERT_TRUE(conn->WriteAll(header, 4));
  MessageChannel ch(*conn);
  auto r = ch.Receive(5000);
  // The server answers bad_message (best effort) and closes.
  if (r.status == MessageChannel::Received::kMessage) {
    EXPECT_EQ(std::get<Error>(*r.message).code, "bad_message");
    r = ch.Receive(5000);
  }
  EXPECT_EQ(r.status, MessageChannel::Received::kClosed);
}

TEST_F(ControlTest, IdleConnectionGetsPingedThenClosed) {
  server_.reset();
  ControlServerOptions opts;
  opts.bind_address = "127.0.0.1";
  opts.port = 0;
  opts.ping_interval_ms = 100;
  opts.idle_timeout_ms = 400;
  ControlServer server(delegate_, Identity(), opts);
  std::string error;
  ASSERT_TRUE(server.Start(&error)) << error;
  auto conn = TlsConnect("127.0.0.1", server.port(), 5000, &error);
  ASSERT_TRUE(conn) << error;
  MessageChannel ch(*conn);
  auto r = ch.Receive(5000);
  ASSERT_EQ(r.status, MessageChannel::Received::kMessage);
  EXPECT_TRUE(std::holds_alternative<Ping>(*r.message));
  // Never answer: eventually bye idle_timeout.
  for (;;) {
    r = ch.Receive(5000);
    ASSERT_EQ(r.status, MessageChannel::Received::kMessage);
    if (auto* bye = std::get_if<Bye>(&*r.message)) {
      EXPECT_EQ(bye->reason, "idle_timeout");
      break;
    }
  }
}

TEST_F(ControlTest, StopWithOpenSession) {
  auto r = ConnectAndStartSession(Options());
  ASSERT_EQ(r.error, "") << r.detail;
  server_->Stop();
  EXPECT_EQ(server_->active_connections(), 0);
  std::lock_guard lock(delegate_.mu);
  EXPECT_EQ(delegate_.ends, 1);
}

TEST_F(ControlTest, CloseConnectionsEndsSessionAndKeepsListening) {
  auto r = ConnectAndStartSession(Options());
  ASSERT_EQ(r.error, "") << r.detail;
  {
    std::lock_guard lock(delegate_.mu);
    EXPECT_EQ(delegate_.last_origin.peer_ip, "127.0.0.1");
    EXPECT_EQ(delegate_.last_origin.username, "alice");
  }
  server_->CloseConnections();
  for (int i = 0; i < 100 && server_->active_connections() > 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(server_->active_connections(), 0);
  {
    std::lock_guard lock(delegate_.mu);
    EXPECT_EQ(delegate_.ends, 1);
  }
  // The client is told why, then the connection is gone.
  auto got = r.connection->channel().Receive(2000);
  ASSERT_EQ(got.status, MessageChannel::Received::kMessage);
  EXPECT_EQ(std::get<Bye>(*got.message).reason, "server_disconnected");
  EXPECT_EQ(r.connection->channel().Receive(2000).status, MessageChannel::Received::kClosed);
  // And the next client gets in.
  auto again = ConnectAndStartSession(Options());
  EXPECT_EQ(again.error, "") << again.detail;
}

TEST(ControlServer, PortInUse) {
  FakeDelegate d;
  ControlServerOptions opts;
  opts.bind_address = "127.0.0.1";
  opts.port = 0;
  ControlServer a(d, Identity(), opts);
  std::string error;
  ASSERT_TRUE(a.Start(&error)) << error;
  opts.port = a.port();
  ControlServer b(d, Identity(), opts);
  EXPECT_FALSE(b.Start(&error));
  EXPECT_NE(error.find("listen"), std::string::npos);
}

namespace {

// The bye reason the client hears next (empty if the connection just closes).
std::string NextBye(rdm::ClientConnection& c) {
  for (int i = 0; i < 40; ++i) {
    auto r = c.channel().Receive(100);
    if (r.status == rdm::MessageChannel::Received::kClosed) return "";
    if (r.status == rdm::MessageChannel::Received::kMessage) {
      if (auto* bye = std::get_if<rdm::Bye>(&*r.message)) return bye->reason;
    }
  }
  return "(nothing)";
}

}  // namespace

TEST_F(ControlTest, DisconnectFromTheServerSaysSo) {
  auto r = ConnectAndStartSession(Options());
  ASSERT_EQ(r.error, "") << r.detail;
  server_->CloseConnections();
  EXPECT_EQ(NextBye(*r.connection), "server_disconnected");
}

TEST_F(ControlTest, StoppingTheServerSaysSo) {
  auto r = ConnectAndStartSession(Options());
  ASSERT_EQ(r.error, "") << r.detail;
  std::thread stop([&] { server_->Stop(); });
  EXPECT_EQ(NextBye(*r.connection), "server_shutdown");
  stop.join();
}

TEST_F(ControlTest, BothEndsKnowTheAddressPairOfTheConnection) {
  auto r = ConnectAndStartSession(Options());
  ASSERT_EQ(r.error, "") << r.detail;
  EXPECT_EQ(r.connection->peer_ip(), "127.0.0.1");  // where the picture goes
  std::lock_guard lock(delegate_.mu);
  EXPECT_EQ(delegate_.last_origin.local_ip, "127.0.0.1");  // where it is served from
}
