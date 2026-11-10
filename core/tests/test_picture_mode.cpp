#include <gtest/gtest.h>

#include "rdmachine/picture_mode.h"

TEST(PictureMode, RoundTrip) {
  for (bool on : {false, true}) {
    const auto m = rdm::DecodePictureMode(rdm::EncodePictureMode({on}));
    ASSERT_TRUE(m);
    EXPECT_EQ(m->text_mode, on);
  }
  EXPECT_EQ(rdm::EncodePictureMode({true}), R"({"text_mode":true})");
}

TEST(PictureMode, UnknownOrMistypedFieldsKeepDefaults) {
  auto m = rdm::DecodePictureMode(R"({"text_mode":"yes","later":1})");
  ASSERT_TRUE(m);
  EXPECT_FALSE(m->text_mode);
  EXPECT_TRUE(rdm::DecodePictureMode("{}"));
  EXPECT_FALSE(rdm::DecodePictureMode("[true]"));
  EXPECT_FALSE(rdm::DecodePictureMode("not json"));
  EXPECT_FALSE(rdm::DecodePictureMode(""));
}
