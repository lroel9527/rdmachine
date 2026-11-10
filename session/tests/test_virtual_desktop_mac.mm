// The real virtual display. It rearranges this Mac's displays for a few
// seconds (the real ones mirror it), so it only runs when asked:
// RDM_TEST_VIRTUAL_DESKTOP=1.
#include <gtest/gtest.h>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <thread>

#include <rdaccel/rdaccel.h>

#include "rdmachine/session/virtual_desktop.h"

namespace {

using rdm::DesktopSpec;

struct Mode {
  size_t points_wide = 0, pixels_wide = 0, pixels_high = 0;
};
Mode ModeOf(CGDirectDisplayID id) {
  Mode m;
  if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
    m = {CGDisplayModeGetWidth(mode), CGDisplayModeGetPixelWidth(mode), CGDisplayModeGetPixelHeight(mode)};
    CGDisplayModeRelease(mode);
  }
  return m;
}

// rdaccel's display 0, in pixels.
bool FirstDisplay(int32_t* w, int32_t* h) {
  rda_display_info info[8];
  info[0].struct_size = sizeof(rda_display_info);
  size_t count = 0;
  if (rda_list_displays(info, 8, &count) != RDA_OK || count == 0) return false;
  *w = info[0].width;
  *h = info[0].height;
  return true;
}

// Runs `body` on another thread while this one (the main thread) runs
// AppKit's event loop, as the server does: only through that loop does a
// process learn the modes of a display that has just appeared.
void WithAppKitLoop(const std::function<void()>& body) {
  [NSApplication sharedApplication];
  std::atomic<bool> done{false};
  std::thread worker([&] {
    body();
    done = true;
  });
  while (!done) {
    @autoreleasepool {
      NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:YES];
      if (e) [NSApp sendEvent:e];
    }
  }
  worker.join();
}

void BuildResizeRestore();

TEST(VirtualDesktopMac, BuildsResizesAndRestores) {
  if (!std::getenv("RDM_TEST_VIRTUAL_DESKTOP")) GTEST_SKIP() << "set RDM_TEST_VIRTUAL_DESKTOP=1 (rearranges the displays)";
  WithAppKitLoop(&BuildResizeRestore);
}

void BuildResizeRestore() {
  const CGDirectDisplayID main_before = CGMainDisplayID();
  const CGRect bounds_before = CGDisplayBounds(main_before);
  uint32_t active_before = 0;
  CGGetActiveDisplayList(0, nullptr, &active_before);

  auto factory = rdm::SystemVirtualDesktop();
  ASSERT_TRUE(factory);
  std::string error;
  {
    // HiDPI: 1440 x 900 points of 2 pixels.
    auto desktop = factory(DesktopSpec{2880, 1800, 2.0}, &error);
    ASSERT_TRUE(desktop) << error;
    EXPECT_EQ(desktop->spec(), (DesktopSpec{2880, 1800, 2.0}));
    const CGDirectDisplayID id = CGMainDisplayID();
    EXPECT_NE(id, main_before);
    Mode m = ModeOf(id);
    EXPECT_EQ(m.points_wide, 1440u);
    EXPECT_EQ(m.pixels_wide, 2880u);
    EXPECT_EQ(m.pixels_high, 1800u);
    // The real displays mirror it: still one desktop, and it is rdaccel's display 0.
    if (main_before != kCGNullDirectDisplay) EXPECT_EQ(CGDisplayMirrorsDisplay(main_before), id);
    int32_t w = 0, h = 0;
    ASSERT_TRUE(FirstDisplay(&w, &h));
    EXPECT_EQ(w, 2880);
    EXPECT_EQ(h, 1800);

    // Another size, in place: the same display.
    ASSERT_TRUE(desktop->Resize(DesktopSpec{2048, 1536, 2.0}, &error)) << error;
    EXPECT_EQ(CGMainDisplayID(), id);
    m = ModeOf(id);
    EXPECT_EQ(m.points_wide, 1024u);
    EXPECT_EQ(m.pixels_wide, 2048u);
    ASSERT_TRUE(FirstDisplay(&w, &h));
    EXPECT_EQ(w, 2048);
    EXPECT_EQ(h, 1536);

    // And without HiDPI.
    ASSERT_TRUE(desktop->Resize(DesktopSpec{1600, 1000, 1.0}, &error)) << error;
    EXPECT_EQ(CGMainDisplayID(), id);
    m = ModeOf(id);
    EXPECT_EQ(m.points_wide, 1600u);
    EXPECT_EQ(m.pixels_wide, 1600u);
    EXPECT_EQ(desktop->spec(), (DesktopSpec{1600, 1000, 1.0}));
  }
  // Gone, and everything as before. (Sizes from the bounds: they are always known.)
  bool restored = false;
  for (int i = 0; i < 200 && !restored; ++i) {
    uint32_t active = 0;
    CGGetActiveDisplayList(0, nullptr, &active);
    const CGRect b = CGDisplayBounds(CGMainDisplayID());
    restored = CGMainDisplayID() == main_before && active == active_before &&
               CGSizeEqualToSize(b.size, bounds_before.size);
    if (!restored) usleep(20000);
  }
  EXPECT_TRUE(restored);
  if (main_before != kCGNullDirectDisplay) EXPECT_EQ(CGDisplayMirrorsDisplay(main_before), kCGNullDirectDisplay);
}

}  // namespace
