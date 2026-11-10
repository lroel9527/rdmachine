#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "rdmachine/desktop.h"

namespace {

using rdm::DesktopSpec;

TEST(Desktop, PlanKeepsARetinaRequestAsItIs) {
  auto d = rdm::PlanDesktop({2880, 1800, 2.0});
  ASSERT_TRUE(d);
  EXPECT_EQ(*d, (DesktopSpec{2880, 1800, 2.0}));
  EXPECT_EQ(rdm::DesktopPointsWide(*d), 1440);
  EXPECT_EQ(rdm::DesktopPointsHigh(*d), 900);

  d = rdm::PlanDesktop({1920, 1080, 1.0});
  ASSERT_TRUE(d);
  EXPECT_EQ(*d, (DesktopSpec{1920, 1080, 1.0}));
  EXPECT_EQ(rdm::DesktopPointsWide(*d), 1920);
}

TEST(Desktop, PlanMakesTheScaleOneOrTwoAndKeepsThePixels) {
  // Windows at 150%, a 3x phone: the pixels stay, a point becomes 2 pixels.
  EXPECT_EQ(*rdm::PlanDesktop({2560, 1440, 1.5}), (DesktopSpec{2560, 1440, 2.0}));
  EXPECT_EQ(*rdm::PlanDesktop({2556, 1178, 3.0}), (DesktopSpec{2556, 1178, 2.0}));
  EXPECT_EQ(*rdm::PlanDesktop({1920, 1080, 1.25}), (DesktopSpec{1920, 1080, 1.0}));
}

TEST(Desktop, PlanMakesSizesEvenAndWithinLimits) {
  // Odd sizes lose a pixel (a whole number of points; video needs even sizes).
  EXPECT_EQ(*rdm::PlanDesktop({2879, 1801, 2.0}), (DesktopSpec{2878, 1800, 2.0}));
  EXPECT_EQ(*rdm::PlanDesktop({1365, 767, 1.0}), (DesktopSpec{1364, 766, 1.0}));
  // Too small: the minimum is in points.
  EXPECT_EQ(*rdm::PlanDesktop({100, 50, 2.0}), (DesktopSpec{640, 640, 2.0}));
  EXPECT_EQ(*rdm::PlanDesktop({100, 50, 1.0}), (DesktopSpec{320, 320, 1.0}));
  // Too large.
  EXPECT_EQ(*rdm::PlanDesktop({20000, 9000, 2.0}),
            (DesktopSpec{rdm::kMaxDesktopWidth, rdm::kMaxDesktopHeight, 2.0}));
}

TEST(Desktop, PlanRefusesNonsense) {
  EXPECT_FALSE(rdm::PlanDesktop({0, 1080, 1.0}));
  EXPECT_FALSE(rdm::PlanDesktop({1920, -1, 1.0}));
  EXPECT_FALSE(rdm::PlanDesktop({1920, 1080, 0.0}));
  EXPECT_FALSE(rdm::PlanDesktop({1920, 1080, -2.0}));
  EXPECT_FALSE(rdm::PlanDesktop({1920, 1080, std::nan("")}));
  EXPECT_FALSE(rdm::PlanDesktop({1920, 1080, std::numeric_limits<double>::infinity()}));
}

TEST(Desktop, MessageRoundTrip) {
  const DesktopSpec d{3024, 1964, 2.0};
  auto out = rdm::DecodeDesktopSpec(rdm::EncodeDesktopSpec(d));
  ASSERT_TRUE(out);
  EXPECT_EQ(*out, d);
  EXPECT_EQ(*rdm::DecodeDesktopSpec(R"({"width":800,"height":600})"), (DesktopSpec{800, 600, 1.0}));
  for (const char* bad : {"", "nope", "[]", "{}", R"({"width":800})", R"({"width":"800","height":600})",
                          R"({"width":800,"height":0})", R"({"width":800,"height":600,"scale":0})",
                          R"({"width":800,"height":600,"scale":"2"})", R"({"width":99999999,"height":600})"}) {
    EXPECT_FALSE(rdm::DecodeDesktopSpec(bad)) << bad;
  }
}

TEST(Desktop, TextForm) {
  EXPECT_EQ(rdm::FormatDesktopSpec({2880, 1800, 2.0}), "2880x1800@2");
  EXPECT_EQ(*rdm::ParseDesktopSpec("2880x1800@2"), (DesktopSpec{2880, 1800, 2.0}));
  EXPECT_EQ(*rdm::ParseDesktopSpec("1920x1080"), (DesktopSpec{1920, 1080, 1.0}));
  EXPECT_EQ(*rdm::ParseDesktopSpec("2560x1440@1.5"), (DesktopSpec{2560, 1440, 1.5}));
  for (const char* bad : {"", "1920", "1920x", "x1080", "1920x1080@", "1920x1080@0", "0x1080", "1920x1080@2x"}) {
    EXPECT_FALSE(rdm::ParseDesktopSpec(bad)) << bad;
  }
}

}  // namespace
