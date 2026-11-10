#include <gtest/gtest.h>

#include "rdmachine/messages.h"

namespace {

template <typename T>
T RoundTrip(const T& m) {
  const std::string text = rdm::SerializeMessage(m);
  rdm::ParseResult r = rdm::ParseMessage(text);
  EXPECT_TRUE(r.message) << r.error << " in " << text;
  EXPECT_TRUE(std::holds_alternative<T>(*r.message)) << text;
  return std::get<T>(*r.message);
}

TEST(Messages, HelloRoundTrip) {
  rdm::ClientHello h{rdm::kProtocolVersion, "studio-mac", "0.1.0"};
  auto out = RoundTrip(h);
  EXPECT_EQ(out.protocol, 0);
  EXPECT_EQ(out.client_name, "studio-mac");
  EXPECT_EQ(out.client_version, "0.1.0");
}

TEST(Messages, ServerHelloSkipsUnknownAuthMethods) {
  auto r = rdm::ParseMessage(
      R"({"type":"server_hello","protocol":0,"server_name":"mbp","server_version":"0.1.0",)"
      R"("auth_methods":["passkey","pairing_code","password"]})");
  ASSERT_TRUE(r.message) << r.error;
  const auto& s = std::get<rdm::ServerHello>(*r.message);
  ASSERT_EQ(s.auth_methods.size(), 2u);
  EXPECT_EQ(s.auth_methods[0], rdm::AuthMethod::kPairingCode);
  EXPECT_EQ(s.auth_methods[1], rdm::AuthMethod::kPassword);
}

TEST(Messages, AuthCarriesOnlyTheFieldsOfItsMethod) {
  rdm::Auth pw{rdm::AuthMethod::kPassword, "alice", "s3cret", ""};
  auto out = RoundTrip(pw);
  EXPECT_EQ(out.username, "alice");
  EXPECT_EQ(out.password, "s3cret");

  rdm::Auth code{rdm::AuthMethod::kPairingCode, "", "", "4821-0937"};
  const std::string text = rdm::SerializeMessage(code);
  EXPECT_EQ(text.find("password"), std::string::npos);
  EXPECT_EQ(RoundTrip(code).code, "4821-0937");

  EXPECT_FALSE(rdm::ParseMessage(R"({"type":"auth","method":"password","username":"a"})").message);
  EXPECT_FALSE(rdm::ParseMessage(R"({"type":"auth","method":"telepathy"})").message);
}

TEST(Messages, AuthResult) {
  auto ok = RoundTrip(rdm::AuthResult{true, "", std::nullopt});
  EXPECT_TRUE(ok.ok);
  auto limited = RoundTrip(rdm::AuthResult{false, "rate_limited", 30000});
  EXPECT_FALSE(limited.ok);
  EXPECT_EQ(limited.error, "rate_limited");
  EXPECT_EQ(limited.retry_after_ms, 30000);
  // A refusal must say why.
  EXPECT_FALSE(rdm::ParseMessage(R"({"type":"auth_result","ok":false})").message);
}

TEST(Messages, SessionRequestDefaults) {
  auto r = rdm::ParseMessage(R"({"type":"session_request"})");
  ASSERT_TRUE(r.message) << r.error;
  const auto& s = std::get<rdm::SessionRequest>(*r.message);
  EXPECT_EQ(s.display_index, 0);
  EXPECT_EQ(s.transport, rdm::MediaTransport::kUdp);
  EXPECT_FALSE(s.view_only);
  auto out = RoundTrip(rdm::SessionRequest{1, rdm::MediaTransport::kTcp, true, std::nullopt});
  EXPECT_EQ(out.display_index, 1);
  EXPECT_EQ(out.transport, rdm::MediaTransport::kTcp);
  EXPECT_TRUE(out.view_only);
}

TEST(Messages, SessionRequestAndReadyCarryTheDesktop) {
  rdm::SessionRequest q;
  q.desktop = rdm::DesktopSpec{2880, 1800, 2.0};
  auto out = RoundTrip(q);
  ASSERT_TRUE(out.desktop);
  EXPECT_EQ(*out.desktop, *q.desktop);
  EXPECT_FALSE(RoundTrip(rdm::SessionRequest{}).desktop);

  // The scale may be left out (1); a desktop without a size is malformed.
  auto r = rdm::ParseMessage(R"({"type":"session_request","desktop":{"width":1280,"height":720}})");
  ASSERT_TRUE(r.message) << r.error;
  EXPECT_EQ(*std::get<rdm::SessionRequest>(*r.message).desktop, (rdm::DesktopSpec{1280, 720, 1.0}));
  for (const char* bad : {R"({"type":"session_request","desktop":7})",
                          R"({"type":"session_request","desktop":{"width":0,"height":720}})",
                          R"({"type":"session_request","desktop":{"width":1280}})",
                          R"({"type":"session_request","desktop":{"width":1280,"height":720,"scale":0}})",
                          R"({"type":"session_request","desktop":{"width":1280,"height":720,"scale":"2"}})"}) {
    auto b = rdm::ParseMessage(bad);
    EXPECT_FALSE(b.message) << bad;
    EXPECT_NE(b.error.find("desktop"), std::string::npos) << b.error;
  }

  rdm::SessionReady s;
  s.port = 4100;
  EXPECT_FALSE(RoundTrip(s).desktop);
  s.desktop = rdm::DesktopSpec{2560, 1440, 2.0};
  auto ready = RoundTrip(s);
  ASSERT_TRUE(ready.desktop);
  EXPECT_EQ(*ready.desktop, *s.desktop);
}

TEST(Messages, SessionReadyCarriesKeyAndToken) {
  rdm::SessionReady s;
  s.port = 4100;
  for (size_t i = 0; i < s.key.size(); ++i) s.key[i] = uint8_t(i * 7 + 1);
  for (size_t i = 0; i < s.auth_token.size(); ++i) s.auth_token[i] = uint8_t(255 - i);
  s.displays.push_back({0, "Built-in Retina Display", 2880, 1800, 2.0});
  auto out = RoundTrip(s);
  EXPECT_EQ(out.transport, rdm::MediaTransport::kUdp);
  EXPECT_EQ(out.port, 4100);
  EXPECT_EQ(out.key, s.key);
  EXPECT_EQ(out.auth_token, s.auth_token);
  ASSERT_EQ(out.displays.size(), 1u);
  EXPECT_EQ(out.displays[0].name, "Built-in Retina Display");
  EXPECT_EQ(out.displays[0].width, 2880);
  EXPECT_DOUBLE_EQ(out.displays[0].scale, 2.0);
}

TEST(Messages, SessionReadyRejectsShortKeyAndBadPort) {
  const std::string token = std::string(43, 'A') + "=";  // 32 zero bytes
  const std::string short_key = "AAAA";                   // 3 bytes
  auto r = rdm::ParseMessage(R"({"type":"session_ready","transport":"udp","port":4100,"key":")" + short_key +
                             R"(","auth_token":")" + token + R"(","displays":[]})");
  EXPECT_FALSE(r.message);
  EXPECT_NE(r.error.find("key"), std::string::npos) << r.error;

  r = rdm::ParseMessage(R"({"type":"session_ready","transport":"udp","port":70000,"key":")" + token +
                        R"(","auth_token":")" + token + R"(","displays":[]})");
  EXPECT_FALSE(r.message);
  EXPECT_NE(r.error.find("port"), std::string::npos) << r.error;
}

TEST(Messages, PingPongByeError) {
  EXPECT_EQ(RoundTrip(rdm::Ping{42}).id, 42);
  EXPECT_EQ(RoundTrip(rdm::Pong{42}).id, 42);
  EXPECT_EQ(RoundTrip(rdm::Bye{"user_closed"}).reason, "user_closed");
  auto e = RoundTrip(rdm::Error{"unsupported", "type: unknown"});
  EXPECT_EQ(e.code, "unsupported");
}

TEST(Messages, IgnoresUnknownFields) {
  auto r = rdm::ParseMessage(R"({"type":"ping","id":1,"from_the_future":{"x":[1,2]}})");
  ASSERT_TRUE(r.message) << r.error;
  EXPECT_EQ(std::get<rdm::Ping>(*r.message).id, 1);
}

TEST(Messages, RejectsGarbage) {
  for (const char* bad : {"", "nope", "[]", "{}", R"({"type":7})", R"({"type":"teleport"})",
                          R"({"type":"hello","protocol":-1,"client_name":"a","client_version":"b"})",
                          R"({"type":"ping","id":"1"})"}) {
    auto r = rdm::ParseMessage(bad);
    EXPECT_FALSE(r.message) << bad;
    EXPECT_FALSE(r.error.empty()) << bad;
  }
}

}  // namespace
