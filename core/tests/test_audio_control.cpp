#include <gtest/gtest.h>

#include "rdmachine/audio_control.h"

TEST(AudioControl, RoundTrip) {
  for (bool paused : {false, true}) {
    const auto m = rdm::DecodeAudioControl(rdm::EncodeAudioControl({paused}));
    ASSERT_TRUE(m);
    EXPECT_EQ(m->paused, paused);
  }
  EXPECT_EQ(rdm::EncodeAudioControl({false}), R"({"paused":false})");
  EXPECT_EQ(rdm::kAudioControlMessageType, 0x10013u);
}

TEST(AudioControl, MissingUnknownOrMistypedFieldsKeepDefaults) {
  auto m = rdm::DecodeAudioControl(R"({"paused":"yes","later":1})");
  ASSERT_TRUE(m);
  EXPECT_FALSE(m->paused);
  m = rdm::DecodeAudioControl(R"({"paused":true,"volume":0.5})");
  ASSERT_TRUE(m);
  EXPECT_TRUE(m->paused);
  m = rdm::DecodeAudioControl("{}");
  ASSERT_TRUE(m);
  EXPECT_FALSE(m->paused);
  EXPECT_FALSE(rdm::DecodeAudioControl("[true]"));
  EXPECT_FALSE(rdm::DecodeAudioControl("not json"));
  EXPECT_FALSE(rdm::DecodeAudioControl(""));
}

TEST(AudioControl, TheOverlaySaysWhyThereIsNoSound) {
  using rdm::AudioOffLine;
  EXPECT_EQ(AudioOffLine(true, "", 3, true, true), "");
  // Only a server below revision 3 is an older one.
  EXPECT_EQ(AudioOffLine(false, "", 2, true, true), "Audio: off (older server)");
  EXPECT_EQ(AudioOffLine(false, "disabled", 3, true, true), "Audio: off (server setting)");
  EXPECT_EQ(AudioOffLine(false, "login_window", 3, true, true), "Audio: off (login window)");
  EXPECT_EQ(AudioOffLine(false, "tcp", 3, true, true), "Audio: off (over TCP there is no sound)");
  EXPECT_EQ(AudioOffLine(false, "unsupported", 3, true, true), "Audio: off (server cannot capture)");
  EXPECT_EQ(AudioOffLine(false, "", 3, false, false), "Audio: off (turned off here)");
  // Started with sound off, View > Play Sound turned on since: not "older server".
  EXPECT_EQ(AudioOffLine(false, "", 3, false, true), "Audio: off (reconnect to hear sound)");
}
