#include <gtest/gtest.h>

#include "rdmachine/session/live_stats.h"

using namespace rdm;
using namespace std::chrono_literals;

namespace {

rda_host_stats Sample(uint64_t frames, uint64_t bytes, uint64_t sent, uint64_t lost) {
  rda_host_stats s{};
  s.struct_size = sizeof(s);
  s.frames_encoded = frames;
  s.frames_captured = frames * 2;
  s.bytes_sent = bytes;
  s.packets_sent = sent;
  s.packets_lost = lost;
  s.rtt_us = 12'500;
  s.target_bitrate_bps = 4'000'000;
  return s;
}

}  // namespace

TEST(LiveStats, RatesBetweenSamples) {
  LiveStatsSampler sampler;
  const auto t0 = LiveStatsSampler::Clock::time_point() + 100s;
  LiveStats first = sampler.Update(Sample(0, 0, 0, 0), t0);
  EXPECT_FALSE(first.valid);
  EXPECT_LT(first.recent_loss_pct, 0);  // nothing measured yet

  LiveStats s = sampler.Update(Sample(30, 250'000, 1000, 20), t0 + 500ms);
  ASSERT_TRUE(s.valid);
  EXPECT_DOUBLE_EQ(s.encoded_fps, 60);
  EXPECT_DOUBLE_EQ(s.captured_fps, 120);
  EXPECT_DOUBLE_EQ(s.bitrate_kbps, 4000);
  EXPECT_DOUBLE_EQ(s.rtt_ms, 12.5);
  EXPECT_DOUBLE_EQ(s.target_kbps, 4000);
  EXPECT_DOUBLE_EQ(s.recent_loss_pct, 2);
  EXPECT_DOUBLE_EQ(s.total_loss_pct, 2);

  // 1000 more packets, none lost: recent is clean, the total remembers.
  s = sampler.Update(Sample(60, 500'000, 2000, 20), t0 + 1000ms);
  EXPECT_DOUBLE_EQ(s.recent_loss_pct, 0);
  EXPECT_DOUBLE_EQ(s.total_loss_pct, 1);
}

TEST(LiveStats, LateIsNotLost) {
  // 50 packets resent, 40 of them only late (jitter): 10 really lost.
  LiveStatsSampler sampler;
  const auto t0 = LiveStatsSampler::Clock::time_point() + 100s;
  sampler.Update(Sample(0, 0, 0, 0), t0);
  rda_host_stats s = Sample(30, 1000, 1000, 50);
  s.packets_spurious = 40;
  LiveStats out = sampler.Update(s, t0 + 1s);
  EXPECT_DOUBLE_EQ(out.recent_loss_pct, 1);
  EXPECT_DOUBLE_EQ(out.total_loss_pct, 1);
}

TEST(LiveStats, StaticScreenKeepsLastLossFigure) {
  LiveStatsSampler sampler;
  const auto t0 = LiveStatsSampler::Clock::time_point() + 100s;
  sampler.Update(Sample(0, 0, 100, 0), t0);
  sampler.Update(Sample(10, 1000, 200, 10), t0 + 1s);
  LiveStats s = sampler.Update(Sample(10, 1000, 200, 10), t0 + 2s);  // nothing sent
  EXPECT_DOUBLE_EQ(s.encoded_fps, 0);
  EXPECT_DOUBLE_EQ(s.recent_loss_pct, 10);
}

TEST(LiveStats, TcpHasNoLossFigures) {
  LiveStatsSampler sampler;
  const auto t0 = LiveStatsSampler::Clock::time_point() + 100s;
  sampler.Update(Sample(0, 0, 0, 0), t0);
  LiveStats s = sampler.Update(Sample(5, 100, 0, 0), t0 + 1s);
  EXPECT_LT(s.recent_loss_pct, 0);
  EXPECT_LT(s.total_loss_pct, 0);
}

TEST(LiveStats, CountersStartingOverDoNotGoNegative) {
  LiveStatsSampler sampler;
  const auto t0 = LiveStatsSampler::Clock::time_point() + 100s;
  sampler.Update(Sample(1000, 1'000'000, 5000, 50), t0);
  LiveStats s = sampler.Update(Sample(10, 1000, 100, 1), t0 + 1s);
  EXPECT_DOUBLE_EQ(s.encoded_fps, 10);
  EXPECT_DOUBLE_EQ(s.recent_loss_pct, 1);
}
