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
