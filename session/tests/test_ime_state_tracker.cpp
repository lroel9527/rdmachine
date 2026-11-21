// What the server tells the client about its input method, built from
// RDMachine Input's events (session/ime_state_tracker.h): the orders macOS
// really sends them in (docs/ime.md), late reports, reports that answer an
// older message, and a composition the system ended.
#include <gtest/gtest.h>

#include "rdmachine/session/ime_state_tracker.h"

using namespace rdm;

namespace {

ImeStateTracker::Report At(int32_t x, uint64_t ctl, uint64_t answers) {
  ImeStateTracker::Report r;
  r.active = true;
  r.ctl = ctl;
  r.app = "com.anthropic.claudefordesktop";
  r.answers = answers;
  r.caret = ImeRect{0, x, 719, 1, 19};
  return r;
}

}  // namespace

TEST(ImeStateTracker, TheSameFieldLetGoAndTakenAgainKeepsItsCaret) {
  ImeStateTracker t;
  t.Reset(false, "", 0);
  t.OnField(true, "app", 7);
  ASSERT_TRUE(t.OnReport(At(364, 7, 0), 0));
  const uint32_t field = t.state().field;
  // Claude, after a reconnect: deactivated and activated within a millisecond.
  t.OnField(false, "app", 7);
  EXPECT_FALSE(t.state().client);
  ASSERT_TRUE(t.state().caret);  // kept for when it comes back
  t.OnField(true, "app", 7);
  EXPECT_TRUE(t.state().client);
  ASSERT_TRUE(t.state().caret);
  EXPECT_EQ(t.state().caret->x, 364);
  EXPECT_EQ(t.state().field, field);
}

TEST(ImeStateTracker, AnotherFieldStartsWithNothingKnown) {
  ImeStateTracker t;
  t.Reset(true, "com.apple.finder", 3);
  ASSERT_TRUE(t.OnReport(At(5, 3, 0), 0));
  const uint32_t field = t.state().field;
  // Finder to Claude: Claude's controller comes before Finder's lets go.
  t.OnField(true, "com.anthropic.claudefordesktop", 8);
  EXPECT_FALSE(t.state().caret);
  EXPECT_NE(t.state().field, field);
  EXPECT_EQ(t.state().app, "com.anthropic.claudefordesktop");
  // An input method before 6.4.0 names no controller: each activation is another field.
  const uint32_t again = t.state().field;
  t.OnField(true, "x", 0);
  EXPECT_NE(t.state().field, again);
}

TEST(ImeStateTracker, ALateReportOfAFieldWithoutTheFocusDoesNotCount) {
  ImeStateTracker t;
  t.Reset(true, "com.apple.finder", 4);
  ASSERT_TRUE(t.OnReport(At(853, 4, 0), 0));
  ImeStateTracker::Report desktop = At(5, 2, 0);
  desktop.active = false;
  EXPECT_FALSE(t.OnReport(desktop, 0));
  EXPECT_EQ(t.state().caret->x, 853);
}

TEST(ImeStateTracker, AReportWithTheFocusFromAFieldNotSeenActivatedTakesOver) {
  ImeStateTracker t;
  t.Reset(false, "", 0);
  t.OnField(true, "app", 5);
  ASSERT_TRUE(t.OnReport(At(400, 5, 0), 0));
  const uint32_t field = t.state().field;
  ASSERT_TRUE(t.OnReport(At(100, 6, 0), 0));
  EXPECT_NE(t.state().field, field);
  EXPECT_TRUE(t.state().client);
  EXPECT_EQ(t.state().caret->x, 100);
}

TEST(ImeStateTracker, SeqIsWhatTheCaretReflects) {
  ImeStateTracker t;
  t.Reset(true, "app", 1);
  ASSERT_TRUE(t.OnReport(At(364, 1, 4), 9));
  EXPECT_EQ(t.state().seq, 4u);  // the input method's answer, not the last message received (9)
  // No caret found (Chromium while composing): seq stays that of the caret.
  ImeStateTracker::Report none = At(0, 1, 6);
  none.caret.reset();
  ASSERT_TRUE(t.OnReport(none, 9));
  EXPECT_EQ(t.state().seq, 4u);
  EXPECT_EQ(t.state().caret->x, 364);
  // An input method that does not say: the last message received, as before.
  ImeStateTracker::Report old = At(380, 1, 0);
  old.answers.reset();
  ASSERT_TRUE(t.OnReport(old, 9));
  EXPECT_EQ(t.state().seq, 9u);
}

TEST(ImeStateTracker, OnlyTheCompositionUnderWayEndsTheClientsOwn) {
  ImeStateTracker t;
  t.Reset(true, "app", 1);
  t.OnMark(10);
  t.OnMark(11);
  EXPECT_FALSE(t.OnEnded(10));  // a controller that still remembered an older mark
  EXPECT_TRUE(t.OnEnded(11));
  EXPECT_FALSE(t.OnEnded(11));  // over already
  t.OnMark(12);
  t.OnCompositionDone();
  EXPECT_FALSE(t.OnEnded(12));  // committed before the system ended it
  t.OnMark(13);
  EXPECT_TRUE(t.OnEnded(std::nullopt));  // an input method before 6.4.0
}

TEST(ImeStateTracker, AGoneInputMethodTakesItsFieldWithIt) {
  ImeStateTracker t;
  t.Reset(true, "app", 2);
  ASSERT_TRUE(t.OnReport(At(364, 2, 0), 0));
  const uint32_t field = t.state().field;
  t.OnConnection(false);
  EXPECT_FALSE(t.state().client);
  EXPECT_FALSE(t.state().caret);
  EXPECT_NE(t.state().field, field);
  t.OnConnection(true);
  EXPECT_FALSE(t.state().client);  // until it says which field has the focus
}

TEST(ImeStateTracker, ASessionStartsFromWhatWasSaidBeforeIt) {
  ImeStateTracker t;
  t.Reset(true, "com.anthropic.claudefordesktop", 9);
  EXPECT_TRUE(t.state().client);
  EXPECT_EQ(t.state().app, "com.anthropic.claudefordesktop");
  EXPECT_NE(t.state().field, 0u);
  const uint32_t field = t.state().field;
  t.OnField(true, "com.anthropic.claudefordesktop", 9);  // the same controller: the same field
  EXPECT_EQ(t.state().field, field);
}
