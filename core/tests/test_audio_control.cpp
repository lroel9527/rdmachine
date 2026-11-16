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
