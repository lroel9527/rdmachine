// View > 统计信息: which lines the overlay shows (client_support.mm). The picks
// go to this test binary's own defaults, cleared around each test.
#import <Foundation/Foundation.h>
#include <gtest/gtest.h>

#include "client_support.h"

using namespace rdmclient;

namespace {

class StatsFieldsTest : public ::testing::Test {
 protected:
  void SetUp() override { Clear(); }
  void TearDown() override { Clear(); }
  static void Clear() {
    [NSUserDefaults.standardUserDefaults removeObjectForKey:kStatsFieldsKey];
    [NSUserDefaults.standardUserDefaults removeObjectForKey:kStatsFieldsKnownKey];
  }
  // As the menu item does.
  static void Toggle(NSString* key) {
    NSMutableSet* on = [StatsFieldsOn() mutableCopy];
    if ([on containsObject:key]) {
      [on removeObject:key];
    } else {
      [on addObject:key];
    }
    SetStatsFieldsOn(on);
  }
  static bool On(NSArray* keys) { return [StatsFieldsOn() isEqualToSet:[NSSet setWithArray:keys]]; }
};

// 7.2.4: the way the 1x picture is enlarged is a field like the others, on
// unless turned off (until then the line showed whatever the pick).
TEST_F(StatsFieldsTest, TheUpscalerLineIsAFieldOnByDefault) {
  bool listed = false;
  for (const auto& f : kStatsFields) listed |= [f.key isEqualToString:@"upscale"];
  EXPECT_TRUE(listed);
  EXPECT_TRUE([StatsFieldsOn() containsObject:@"upscale"]);
  Toggle(@"upscale");
  EXPECT_FALSE([StatsFieldsOn() containsObject:@"upscale"]);
  Toggle(@"upscale");
  EXPECT_TRUE([StatsFieldsOn() containsObject:@"upscale"]);
}

// Nothing picked yet: the first field turned off stays off, the traffic total
// too (until 7.2.4 it came straight back on).
TEST_F(StatsFieldsTest, TheFirstPickSticks) {
  for (NSString* key in @[ @"traffic", @"upscale", @"fps" ]) {
    Clear();
    Toggle(key);
    EXPECT_FALSE([StatsFieldsOn() containsObject:key]) << key.UTF8String;
  }
}

// A pick made by 6.0 to 7.2.3: the upscaler line stays on once, the rest of
// the pick is kept, and turning the line off then sticks.
TEST_F(StatsFieldsTest, AnOlderPickKeepsTheUpscalerLineOnce) {
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  [d setObject:@[ @"fps", @"bitrate" ] forKey:kStatsFieldsKey];
  [d setObject:@[ @"traffic" ] forKey:kStatsFieldsKnownKey];
  EXPECT_TRUE(On(@[ @"fps", @"bitrate", @"upscale" ]));
  Toggle(@"upscale");
  EXPECT_TRUE(On(@[ @"fps", @"bitrate" ]));
}

// A pick from before 6.0 (nothing known): both later fields come on once.
TEST_F(StatsFieldsTest, APickFromBefore6GetsBothLaterFieldsOnce) {
  [NSUserDefaults.standardUserDefaults setObject:@[ @"latency" ] forKey:kStatsFieldsKey];
  EXPECT_TRUE(On(@[ @"latency", @"traffic", @"upscale" ]));
  Toggle(@"traffic");
  Toggle(@"upscale");
  EXPECT_TRUE(On(@[ @"latency" ]));
}

}  // namespace
