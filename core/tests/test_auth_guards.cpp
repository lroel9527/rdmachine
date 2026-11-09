#include <gtest/gtest.h>

#include "rdmachine/auth_guards.h"

using namespace rdm;
using namespace std::chrono_literals;

namespace {
const Clock::time_point T0 = Clock::time_point() + 1000h;
}

TEST(FailureLimiter, BlocksAfterFiveFailuresInAMinute) {
  FailureLimiter l;
  for (int i = 0; i < 4; ++i) {
    l.RecordFailure("1.2.3.4", T0 + i * 1s);
    EXPECT_FALSE(l.RetryAfter("1.2.3.4", T0 + i * 1s));
  }
  l.RecordFailure("1.2.3.4", T0 + 4s);
  auto wait = l.RetryAfter("1.2.3.4", T0 + 5s);
  ASSERT_TRUE(wait);
  EXPECT_EQ(*wait, 59s);
  EXPECT_FALSE(l.RetryAfter("5.6.7.8", T0 + 5s));  // other sources unaffected
  EXPECT_FALSE(l.RetryAfter("1.2.3.4", T0 + 64s));  // block over
}

TEST(FailureLimiter, OldFailuresFallOutOfTheWindow) {
  FailureLimiter l;
  for (int i = 0; i < 4; ++i) l.RecordFailure("a", T0 + i * 1s);
  l.RecordFailure("a", T0 + 70s);  // the first four are older than a minute
  EXPECT_FALSE(l.RetryAfter("a", T0 + 70s));
}

TEST(FailureLimiter, SuccessClears) {
  FailureLimiter l;
  for (int i = 0; i < 4; ++i) l.RecordFailure("a", T0);
  l.RecordSuccess("a");
  l.RecordFailure("a", T0);
  EXPECT_FALSE(l.RetryAfter("a", T0));
}

TEST(PairingCodes, IssuesEightDigits) {
  PairingCodes p;
  std::string code = p.Issue(T0);
  ASSERT_EQ(code.size(), 8u);
  for (char c : code) EXPECT_TRUE(c >= '0' && c <= '9');
  EXPECT_TRUE(p.active(T0));
}

TEST(PairingCodes, SingleUseAndTolerantOfGrouping) {
  PairingCodes p;
  std::string code = p.Issue(T0);
  EXPECT_EQ(p.remaining(T0 + 1min), 4min);
  std::string grouped = code.substr(0, 4) + " - " + code.substr(4);
  EXPECT_EQ(p.Check(grouped, T0 + 1s), PairingCodes::Result::kOk);
  EXPECT_EQ(p.remaining(T0 + 2s), Clock::duration::zero());  // used
  EXPECT_EQ(p.Check(code, T0 + 2s), PairingCodes::Result::kNone);
}

TEST(PairingCodes, Expires) {
  PairingCodes p;
  std::string code = p.Issue(T0);
  EXPECT_EQ(p.Check(code, T0 + 5min), PairingCodes::Result::kExpired);
  EXPECT_FALSE(p.active(T0 + 5min));
  EXPECT_EQ(p.remaining(T0 + 5min), Clock::duration::zero());
}

TEST(PairingCodes, VoidAfterThreeWrongAttempts) {
  PairingCodes p;
  std::string code = p.Issue(T0);
  std::string wrong = code;
  wrong[0] = wrong[0] == '9' ? '0' : wrong[0] + 1;
  EXPECT_EQ(p.Check(wrong, T0), PairingCodes::Result::kWrong);
  EXPECT_EQ(p.Check("123", T0), PairingCodes::Result::kWrong);
  EXPECT_EQ(p.Check(wrong, T0), PairingCodes::Result::kWrong);
  EXPECT_EQ(p.Check(code, T0), PairingCodes::Result::kNone);
}

TEST(PairingCodes, NewCodeReplacesOld) {
  PairingCodes p;
  std::string a = p.Issue(T0);
  std::string b;
  do b = p.Issue(T0); while (b == a);
  EXPECT_EQ(p.Check(a, T0), PairingCodes::Result::kWrong);
  EXPECT_EQ(p.Check(b, T0), PairingCodes::Result::kOk);
}

TEST(OneTimeTokens, ConsumedOnce) {
  OneTimeTokens t;
  OneTimeTokens::Token a{}, b{};
  a[0] = 1;
  b[0] = 2;
  t.Add(a, T0);
  t.Add(b, T0);
  EXPECT_TRUE(t.Consume(a, T0 + 1s));
  EXPECT_FALSE(t.Consume(a, T0 + 1s));
  EXPECT_EQ(t.size(), 1u);
  EXPECT_TRUE(t.Consume(b, T0 + 1s));
}

TEST(OneTimeTokens, ExpireAndRejectWrongLength) {
  OneTimeTokens t;
  OneTimeTokens::Token a{};
  t.Add(a, T0);
  EXPECT_FALSE(t.Consume(std::span<const uint8_t>(a.data(), 16), T0));
  EXPECT_FALSE(t.Consume(a, T0 + 30s));
  EXPECT_EQ(t.size(), 0u);
}
