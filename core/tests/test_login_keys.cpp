#include "rdmachine/login_keys.h"

#include <gtest/gtest.h>

#include "rdmachine/messages.h"

namespace rdm {
namespace {

using K = LoginKey::Kind;

TEST(LoginKeys, PasswordThenEnter) {
  const auto keys = LoginKeystrokes("lroel", "aB1!", false);
  ASSERT_EQ(keys.size(), 5u);
  EXPECT_EQ(keys[0].kind, K::kKey);
  EXPECT_EQ(keys[0].code, 0x04u);  // a
  EXPECT_FALSE(keys[0].shift);
  EXPECT_EQ(keys[1].code, 0x05u);  // B
  EXPECT_TRUE(keys[1].shift);
  EXPECT_EQ(keys[2].code, 0x1Eu);  // 1
  EXPECT_FALSE(keys[2].shift);
  EXPECT_EQ(keys[3].code, 0x1Eu);  // !
  EXPECT_TRUE(keys[3].shift);
  EXPECT_EQ(keys[4].code, kHidEnter);
}

TEST(LoginKeys, NameTabPasswordEnterWhenTheWindowAsksForTheName) {
  const auto keys = LoginKeystrokes("ab", "c", true);
  ASSERT_EQ(keys.size(), 5u);
  EXPECT_EQ(keys[0].code, 0x04u);
  EXPECT_EQ(keys[1].code, 0x05u);
  EXPECT_EQ(keys[2].code, kHidTab);
  EXPECT_EQ(keys[3].code, 0x06u);
  EXPECT_EQ(keys[4].code, kHidEnter);
}

TEST(LoginKeys, EveryPrintableAsciiHasAKey) {
  for (char c = 0x20; c < 0x7F; ++c) {
    uint32_t usage = 0;
    bool shift = false;
    EXPECT_TRUE(AsciiToHid(c, &usage, &shift)) << "char " << static_cast<int>(c);
    EXPECT_GE(usage, 0x04u);
  }
  uint32_t usage = 0;
  bool shift = false;
  EXPECT_TRUE(AsciiToHid('0', &usage, &shift));
  EXPECT_EQ(usage, 0x27u);
  EXPECT_TRUE(AsciiToHid(')', &usage, &shift));
  EXPECT_EQ(usage, 0x27u);
  EXPECT_TRUE(shift);
}

TEST(LoginKeys, NonAsciiGoesAsCharacters) {
  const auto keys = LoginKeystrokes("", "密a😀", false);
  ASSERT_EQ(keys.size(), 4u);
  EXPECT_EQ(keys[0].kind, K::kText);
  EXPECT_EQ(keys[0].code, 0x5BC6u);
  EXPECT_EQ(keys[1].kind, K::kKey);
  EXPECT_EQ(keys[2].kind, K::kText);
  EXPECT_EQ(keys[2].code, 0x1F600u);
  EXPECT_EQ(keys[3].code, kHidEnter);
}

TEST(LoginKeys, MalformedUtf8IsSkipped) {
  const auto keys = LoginKeystrokes("", std::string("a\x80\xE5") + "b", false);
  ASSERT_EQ(keys.size(), 3u);  // a, (bad bytes skipped), b, Enter
  EXPECT_EQ(keys[0].code, 0x04u);
  EXPECT_EQ(keys[1].code, 0x05u);
  EXPECT_EQ(keys[2].code, kHidEnter);
}

TEST(SessionReadyLoginWindow, RoundTripsAndDefaultsToFalse) {
  SessionReady r;
  r.port = 4100;
  r.login_window = true;
  auto parsed = ParseMessage(SerializeMessage(Message{r}));
  ASSERT_TRUE(parsed.message) << parsed.error;
  EXPECT_TRUE(std::get<SessionReady>(*parsed.message).login_window);

  r.login_window = false;
  const std::string wire = SerializeMessage(Message{r});
  EXPECT_EQ(wire.find("login_window"), std::string::npos);  // older clients see nothing new
  parsed = ParseMessage(wire);
  ASSERT_TRUE(parsed.message) << parsed.error;
  EXPECT_FALSE(std::get<SessionReady>(*parsed.message).login_window);
}

}  // namespace
}  // namespace rdm
