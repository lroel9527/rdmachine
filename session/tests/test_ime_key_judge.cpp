#include <gtest/gtest.h>

#include "rdmachine/session/ime_key_judge.h"

using rdm::ImeKeyJudge;
using Keys = ImeKeyJudge::Verdict::Keys;

namespace {

ImeKeyJudge::Key Raw(uint64_t id, uint64_t ack, std::optional<bool> field = std::nullopt) {
  return {id, ack, /*im=*/false, field};
}
ImeKeyJudge::Key Im(uint64_t id, uint64_t ack) { return {id, ack, /*im=*/true, true}; }

void ExpectTaken(const ImeKeyJudge::Verdict& v, Keys keys) {
  EXPECT_TRUE(v.messages);
  EXPECT_EQ(v.keys, keys);
  EXPECT_FALSE(v.told);
}

void ExpectDropped(const ImeKeyJudge::Verdict& v) {
  EXPECT_FALSE(v.messages);
  EXPECT_EQ(v.keys, Keys::kNone);
}

}  // namespace

TEST(ImeKeyJudge, KeysGoAsTheClientSentThemWhileItWasRight) {
  ImeKeyJudge j;
  ExpectTaken(j.OnKey(Im(1, 0), true), Keys::kNone);         // composed: its marks go in
  ExpectTaken(j.OnKey(Raw(2, 0), true), Keys::kAsSent);      // its key-up, a shortcut, a modifier
  ExpectTaken(j.OnKey(Raw(3, 0, true), false), Keys::kAsSent);  // its input method gave it back (Return)
  ExpectTaken(j.OnKey(Raw(4, 0, false), false), Keys::kAsSent);  // nothing takes text: a key
  ExpectTaken(j.OnKey(Raw(5, 0), false), Keys::kAsSent);
  EXPECT_EQ(j.back(), 0u);
  EXPECT_EQ(j.typed(), 0u);
  EXPECT_TRUE(j.TakesMessage(0));
}

// 00:31:13: Claude moved its focus back to its text field on the first key;
// the client learnt it a network trip later, and its next keys were letters.
TEST(ImeKeyJudge, AKeySentAsAKeyGoesBackWhereAFieldTakesInputMethods) {
  ImeKeyJudge j;
  ExpectTaken(j.OnKey(Raw(1, 0, false), false), Keys::kAsSent);  // moves the focus: a letter, as on that Mac
  ExpectTaken(j.OnKey(Raw(2, 0), false), Keys::kAsSent);         // its key-up
  const auto v = j.OnKey(Raw(3, 0, false), true);                 // the field has the focus now
  ExpectDropped(v);
  EXPECT_TRUE(v.told);
  EXPECT_EQ(j.back(), 3u);
  // Everything sent before the client heard of it comes again: not used now.
  ExpectDropped(j.OnKey(Raw(4, 0), true));        // key 3's key-up
  ExpectDropped(j.OnKey(Raw(5, 0, false), true));
  ExpectDropped(j.OnKey(Im(6, 0), true));         // the client heard the field and composed
  EXPECT_FALSE(j.TakesMessage(0));                // a cancel of its own, say
  EXPECT_EQ(j.back(), 3u);                        // one back covers them all
  // Handled again, now knowing.
  ExpectTaken(j.OnKey(Im(7, 3), true), Keys::kNone);
  ExpectTaken(j.OnKey(Im(8, 3), true), Keys::kNone);
  EXPECT_TRUE(j.TakesMessage(3));
}

TEST(ImeKeyJudge, AKeyItsInputMethodTookIsTypedWhereNoFieldTakesIt) {
  ImeKeyJudge j;
  ExpectTaken(j.OnKey(Im(1, 0), true), Keys::kNone);
  auto v = j.OnKey(Im(2, 0), false);  // the focus left the text field
  EXPECT_FALSE(v.messages);           // its marks: nowhere to go
  EXPECT_EQ(v.keys, Keys::kTyped);
  EXPECT_TRUE(v.told);
  EXPECT_EQ(j.typed(), 2u);
  // The next ones too, as they come, without telling again.
  v = j.OnKey(Im(3, 0), false);
  EXPECT_FALSE(v.messages);
  EXPECT_EQ(v.keys, Keys::kTyped);
  EXPECT_FALSE(v.told);
  EXPECT_EQ(j.typed(), 2u);
  // A shortcut meanwhile goes in; the commit its input method made for it does not.
  v = j.OnKey(Raw(4, 0), false);
  EXPECT_FALSE(v.messages);
  EXPECT_EQ(v.keys, Keys::kAsSent);
  EXPECT_FALSE(j.TakesMessage(0));
  // Heard: its composition is gone, and the keys go as keys from here.
  EXPECT_TRUE(j.TakesMessage(2));
  ExpectTaken(j.OnKey(Raw(5, 2, false), false), Keys::kAsSent);
}

// Claude's type-to-focus after scrolling: the first key typed as a key moves
// the focus back to the text field.
TEST(ImeKeyJudge, ACompositionBuiltOnTypedKeysGoesBackOnceAFieldTakesInputMethods) {
  ImeKeyJudge j;
  EXPECT_EQ(j.OnKey(Im(1, 0), false).keys, Keys::kTyped);
  EXPECT_EQ(j.typed(), 1u);
  const auto v = j.OnKey(Im(2, 0), true);  // the field took the focus again
  ExpectDropped(v);
  EXPECT_TRUE(v.told);
  EXPECT_EQ(j.back(), 2u);
  ExpectDropped(j.OnKey(Im(3, 0), true));
  ExpectDropped(j.OnKey(Raw(4, 1), true));  // heard typed but not back yet
  EXPECT_FALSE(j.TakesMessage(1));
  ExpectTaken(j.OnKey(Im(5, 2), true), Keys::kNone);
}

TEST(ImeKeyJudge, KeysSentAsKeysAfterAFieldWentAwayGoBackIfItComesBack) {
  ImeKeyJudge j;
  EXPECT_EQ(j.OnKey(Im(1, 0), false).keys, Keys::kTyped);
  // The client heard that nothing takes text (not yet typed) and sends keys as keys.
  ExpectDropped(j.OnKey(Raw(2, 0, false), true));
  EXPECT_EQ(j.back(), 2u);
}

TEST(ImeKeyJudge, ALaterMistakeIsToldAgain) {
  ImeKeyJudge j;
  EXPECT_TRUE(j.OnKey(Raw(1, 0, false), true).told);
  EXPECT_EQ(j.back(), 1u);
  ExpectTaken(j.OnKey(Im(2, 1), true), Keys::kNone);
  auto v = j.OnKey(Im(3, 1), false);
  EXPECT_TRUE(v.told);
  EXPECT_EQ(j.typed(), 3u);
  EXPECT_EQ(j.back(), 1u);
  ExpectTaken(j.OnKey(Raw(4, 3, false), false), Keys::kAsSent);
  v = j.OnKey(Raw(5, 3, false), true);
  EXPECT_TRUE(v.told);
  EXPECT_EQ(j.back(), 5u);
  EXPECT_EQ(j.judged(), 5u);
  j.Reset();
  EXPECT_EQ(j.back(), 0u);
  EXPECT_EQ(j.typed(), 0u);
  EXPECT_EQ(j.judged(), 0u);
  EXPECT_TRUE(j.TakesMessage(0));
}

TEST(ImeKeyJudge, JudgedIsTheLastKeySeenEvenWhenNotUsed) {
  ImeKeyJudge j;
  EXPECT_TRUE(j.OnKey(Raw(4, 0, false), true).told);
  ExpectDropped(j.OnKey(Raw(5, 0), true));
  EXPECT_EQ(j.judged(), 5u);
}
