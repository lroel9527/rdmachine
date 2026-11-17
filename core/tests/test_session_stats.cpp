#include <gtest/gtest.h>

#include "rdmachine/session_stats.h"

using namespace rdm;

TEST(SessionStats, RoundTrip) {
  SessionStatsReport r;
  r.width = 2880;
  r.height = 1800;
  r.encoded_fps = 57.5;
  r.captured_fps = 60;
  r.bitrate_kbps = 3200;
  r.target_kbps = 8000;
  r.bandwidth_kbps = 120000;
  r.rtt_ms = 18.25;
  r.recent_loss_pct = 0.4;
  r.total_loss_pct = 1.5;
  r.frames_dropped_congestion = 3;
  r.encoder = "VideoToolbox HEVC";
  r.input_source = "简体拼音";
  r.view_only = true;
  r.text_mode = true;
  r.audio = "on";
  r.audio_kbps = 162.5;
  auto back = DecodeSessionStats(EncodeSessionStats(r));
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, r);
}

TEST(SessionStats, MissingFieldsKeepDefaultsAndBadInputFails) {
  auto r = DecodeSessionStats(R"({"transport":"udp","rtt_ms":"fast"})");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->rtt_ms, 0);
  EXPECT_LT(r->recent_loss_pct, 0);  // not measured
  // An older server's report has no sound fields.
  EXPECT_EQ(r->audio, "");
  EXPECT_EQ(r->audio_kbps, 0);
  EXPECT_FALSE(DecodeSessionStats("not json"));
  EXPECT_FALSE(DecodeSessionStats("[]"));
}
