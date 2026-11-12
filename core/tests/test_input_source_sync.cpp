#include <gtest/gtest.h>

#include "rdmachine/input_source_sync.h"

using namespace rdm;

namespace {

const InputSourceState kPinyin{"com.apple.inputmethod.SCIM.ITABC", "简体拼音", "zh-Hans", true, false};
const InputSourceState kShuangpin{"com.apple.inputmethod.SCIM.Shuangpin", "双拼", "zh-Hans", true, false};
const InputSourceState kZhuyin{"com.apple.inputmethod.TCIM.Zhuyin", "注音", "zh-Hant", true, false};
const InputSourceState kKana{"com.apple.inputmethod.Kotoeri.RomajiTyping.Japanese", "日本語", "ja", true, false};
const InputSourceState kAbc{"com.apple.keylayout.ABC", "ABC", "en", false, true};
const InputSourceState kUs{"com.apple.keylayout.US", "U.S.", "en", false, true};
const InputSourceState kGerman{"com.apple.keylayout.German", "German", "de", false, true};
const InputSourceState kRussian{"com.apple.keylayout.Russian", "Russian", "ru", false, false};

}  // namespace

TEST(InputSourceSync, RoundTrip) {
  auto back = DecodeInputSource(EncodeInputSource(kPinyin));
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, kPinyin);
}

TEST(InputSourceSync, RejectsBadPayloads) {
  EXPECT_FALSE(DecodeInputSource(""));
  EXPECT_FALSE(DecodeInputSource("not json"));
  EXPECT_FALSE(DecodeInputSource("[1,2]"));
  EXPECT_FALSE(DecodeInputSource(R"({"name":"no id"})"));
  EXPECT_FALSE(DecodeInputSource(R"({"id":""})"));
  EXPECT_FALSE(DecodeInputSource(R"({"id":")" + std::string(300, 'x') + R"("})"));
  // Wrong types are ignored, not fatal.
  auto s = DecodeInputSource(R"({"id":"a","input_method":"yes","language":7})");
  ASSERT_TRUE(s);
  EXPECT_FALSE(s->input_method);
  EXPECT_EQ(s->language, "");
}

TEST(InputSourceSync, SameIdWins) {
  EXPECT_EQ(ChooseInputSource(kShuangpin, {kPinyin, kShuangpin, kAbc}), kShuangpin.id);
  EXPECT_EQ(ChooseInputSource(kUs, {kAbc, kUs}), kUs.id);
}

TEST(InputSourceSync, InputMethodFallsBackByLanguage) {
  // Client uses Shuangpin, server only has Pinyin: both zh-Hans.
  EXPECT_EQ(ChooseInputSource(kShuangpin, {kAbc, kPinyin}), kPinyin.id);
  // Same primary language when nothing matches exactly.
  EXPECT_EQ(ChooseInputSource(kPinyin, {kAbc, kZhuyin}), kZhuyin.id);
  // Exact language beats the same primary one.
  EXPECT_EQ(ChooseInputSource(kShuangpin, {kZhuyin, kPinyin}), kPinyin.id);
  // Another language's input method is no stand-in.
  EXPECT_EQ(ChooseInputSource(kPinyin, {kAbc, kKana}), std::nullopt);
}

TEST(InputSourceSync, PlainLayoutGoesToAnAsciiLayout) {
  EXPECT_EQ(ChooseInputSource(kAbc, {kPinyin, kUs}), kUs.id);
  // The same language first, else any ASCII layout.
  EXPECT_EQ(ChooseInputSource(kUs, {kGerman, kAbc}), kAbc.id);
  EXPECT_EQ(ChooseInputSource(kAbc, {kPinyin, kGerman}), kGerman.id);
  // Never an input method for plain typing.
  EXPECT_EQ(ChooseInputSource(kAbc, {kPinyin}), std::nullopt);
}

TEST(InputSourceSync, NonAsciiLayoutNeedsItsLanguage) {
  const InputSourceState russian2{"com.apple.keylayout.RussianWin", "Russian – PC", "ru", false, false};
  EXPECT_EQ(ChooseInputSource(kRussian, {kAbc, russian2}), russian2.id);
  EXPECT_EQ(ChooseInputSource(kRussian, {kAbc, kPinyin}), std::nullopt);
}

namespace {

rdm::InputSourceState Src(std::string id, std::string lang, bool ime, bool ascii) {
  rdm::InputSourceState s;
  s.id = std::move(id);
  s.language = std::move(lang);
  s.input_method = ime;
  s.ascii_capable = ascii;
  return s;
}

}  // namespace

TEST(InputSourceSync, TheLoginWindowGetsALayoutForAClientOnPinyin) {
  const auto pinyin = Src("com.apple.inputmethod.SCIM.ITABC", "zh-Hans", true, true);
  const std::vector<rdm::InputSourceState> here = {
      pinyin, Src("com.apple.keylayout.US", "en", false, true), Src("com.apple.keylayout.ABC", "en", false, true)};
  // In a user's session the same input method, as before.
  EXPECT_EQ(rdm::ChooseInputSource(pinyin, here), "com.apple.inputmethod.SCIM.ITABC");
  // At the login window: ABC, never the input method.
  EXPECT_EQ(rdm::ChooseInputSource(pinyin, here, true), "com.apple.keylayout.ABC");
  // No ABC: U.S.; neither: any ASCII layout; none at all: nothing.
  EXPECT_EQ(rdm::ChooseInputSource(pinyin, {pinyin, here[1]}, true), "com.apple.keylayout.US");
  const auto dvorak = Src("com.apple.keylayout.Dvorak", "en", false, true);
  EXPECT_EQ(rdm::ChooseInputSource(pinyin, {pinyin, dvorak}, true), "com.apple.keylayout.Dvorak");
  EXPECT_EQ(rdm::ChooseInputSource(pinyin, {pinyin}, true), std::nullopt);
  // A client on a plain layout keeps it at the login window too.
  EXPECT_EQ(rdm::ChooseInputSource(dvorak, {pinyin, here[1], dvorak}, true), "com.apple.keylayout.Dvorak");
}

