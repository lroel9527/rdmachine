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
