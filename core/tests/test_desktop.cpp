#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

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

// A 13-inch MacBook Air: a 2560 x 1664 panel that by default shows
// 1470 x 956 points rendered at 2940 x 1912 and scaled down.
std::vector<rdm::DisplayModeInfo> AirModes() {
  return {
      {1470, 956, 2940, 1912, 60, false},  // 0: the default, scaled
      {1710, 1112, 3420, 2224, 60, false},  // 1: "more space", scaled
      {1280, 832, 2560, 1664, 60, false},   // 2: native 2x
      {1024, 665, 2048, 1330, 60, false},   // 3: larger text
      {2560, 1664, 2560, 1664, 60, true},   // 4: the panel at 1x
      {1280, 832, 1280, 832, 60, false},    // 5: low resolution
  };
}

TEST(Desktop, NativeHiDpiModeIsThePanelAtTwoPixelsPerPoint) {
  EXPECT_EQ(rdm::ChooseNativeHiDpiMode(AirModes(), 60), 2);
  // Nothing is marked native: the largest 1x mode is the panel.
  auto modes = AirModes();
  modes[4].native = false;
  EXPECT_EQ(rdm::ChooseNativeHiDpiMode(modes, 60), 2);
  // A scaled mode with more pixels than the panel is not the panel.
  modes[1].native = false;
  EXPECT_EQ(rdm::ChooseNativeHiDpiMode(modes, 0), 2);
}

TEST(Desktop, NativeHiDpiModeKeepsTheRefreshRate) {
  // A ProMotion panel offers the mode at several rates.
  std::vector<rdm::DisplayModeInfo> modes = {
      {1728, 1117, 3456, 2234, 120, true}, {1728, 1117, 3456, 2234, 60, true}, {1728, 1117, 3456, 2234, 48, true},
      {1512, 982, 3024, 1964, 120, false}};
  EXPECT_EQ(rdm::ChooseNativeHiDpiMode(modes, 60), 1);
  EXPECT_EQ(rdm::ChooseNativeHiDpiMode(modes, 120), 0);
  EXPECT_EQ(rdm::ChooseNativeHiDpiMode(modes, 0), 0);  // not stated: the highest
}

TEST(Desktop, NativeHiDpiModeMayNotExist) {
  EXPECT_EQ(rdm::ChooseNativeHiDpiMode({}, 60), -1);
  // An ordinary 1080p monitor: no 2x mode of its panel.
  EXPECT_EQ(rdm::ChooseNativeHiDpiMode({{1920, 1080, 1920, 1080, 60, true}, {1280, 720, 1280, 720, 60, false}}, 60), -1);
  // An odd panel size has no whole number of points at 2x.
  EXPECT_EQ(rdm::ChooseNativeHiDpiMode({{1365, 767, 1365, 767, 60, true}, {682, 383, 1364, 766, 60, false}}, 60), -1);
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
