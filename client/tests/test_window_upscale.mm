// The window's side of the upscaler (docs/upscaler.md): frames given to a real
// RemoteWindowController, as rdaccel's decode thread gives them, and what its
// layer ends up showing. The window is never put on screen and nothing
// connects anywhere.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "remote_window.h"
#include "upscaler_model.h"

using namespace rdmclient;

namespace {

struct Pic {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
};

// Stripes of text-like strokes that move with `seed`: every pixel position
// differs between seeds somewhere.
Pic Make(int w, int h, int seed) {
  Pic p{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const bool ink = ((x + seed) % 7 < 2 && (y / 9) % 2 == 0) || ((y + seed) % 11 == 3 && x % 13 < 9);
      const uint8_t v = ink ? static_cast<uint8_t>(30 + (x * 7 + y * 3 + seed) % 40) : static_cast<uint8_t>(250 - (x + y) % 5);
      uint8_t* q = &p.px[(static_cast<size_t>(y) * w + x) * 4];
      q[0] = v;
      q[1] = static_cast<uint8_t>(ink && seed % 2 ? 90 : v);
      q[2] = v;
      q[3] = 255;
    }
  }
  return p;
}

Pic CpuModel(const Pic& p) {
  Pic o{2 * p.w, 2 * p.h, std::vector<uint8_t>(static_cast<size_t>(p.w) * p.h * 16)};
  upscaler::UpscaleOnCpu(p.px.data(), p.w, p.h, static_cast<size_t>(p.w) * 4, o.px.data(), static_cast<size_t>(o.w) * 4);
  return o;
}

Pic Read(IOSurfaceRef s) {
  Pic p{static_cast<int>(IOSurfaceGetWidth(s)), static_cast<int>(IOSurfaceGetHeight(s)), {}};
  p.px.resize(static_cast<size_t>(p.w) * p.h * 4);
  IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
  const auto* b = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(s));
  for (int y = 0; y < p.h; ++y) std::memcpy(&p.px[static_cast<size_t>(y) * p.w * 4], b + y * IOSurfaceGetBytesPerRow(s), p.w * 4);
  IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
  return p;
}

int Worst(const Pic& a, const Pic& b) {
  if (a.w != b.w || a.h != b.h) return 256;
  int worst = 0;
  for (size_t i = 0; i < a.px.size(); ++i) worst = std::max(worst, std::abs(a.px[i] - b.px[i]));
  return worst;
}

class WindowUpscaleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!MTLCreateSystemDefaultDevice()) GTEST_SKIP() << "no Metal device";
    [NSApplication sharedApplication];
    [NSUserDefaults.standardUserDefaults removeObjectForKey:kUpscalerKey];  // the default: the model
    decode_ = dispatch_queue_create("decode", DISPATCH_QUEUE_SERIAL);
    win_ = [[RemoteWindowController alloc] initWithTitle:@"test"];
  }
  void TearDown() override {
    if (win_) {
      [win_ endSession];
      [win_.window close];
    }
    [NSUserDefaults.standardUserDefaults removeObjectForKey:kUpscalerKey];
  }
  // A stream of w x h shown at exactly 2x: the window is sized so.
  void Open(int w, int h) {
    [win_ streamInfo:w height:h];
    Spin(0.05);
    const CGFloat backing = win_.window.backingScaleFactor > 0 ? win_.window.backingScaleFactor : 1;
    [win_.window setContentSize:NSMakeSize(2 * w / backing, 2 * h / backing)];
    Spin(0.05);
  }
  // As rdaccel's decode thread: the whole picture and what changed.
  void Feed(const Pic& p, std::vector<rda_rect> changed) {
    rda_image img{};
    img.width = p.w;
    img.height = p.h;
    img.stride = p.w * 4;
    img.data = const_cast<uint8_t*>(p.px.data());
    dispatch_sync(decode_, ^{
      [win_ frame:&img changed:changed.data() count:changed.size()];
    });
  }
  static void Spin(double secs) { [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:secs]]; }
  Pic Shown() {
    id c = win_.window.contentView.layer.contents;
    if (!c) return Pic{};
    return Read((__bridge IOSurfaceRef)c);
  }
  dispatch_queue_t decode_;
  RemoteWindowController* win_ = nil;
};

TEST_F(WindowUpscaleTest, ShowsTheModelsPictureAndKeepsItUpToDate) {
  const int w = 320, h = 200;
  Open(w, h);
  const Pic a = Make(w, h, 1);
  Feed(a, {});
  Spin(0.1);
  EXPECT_LE(Worst(Shown(), CpuModel(a)), 1);
  // A small change: only it (and kReach around it) is redone, with no seam.
  Pic b = a;
  const Pic other = Make(w, h, 4);
  for (int y = 50; y < 90; ++y) std::memcpy(&b.px[(y * w + 100) * 4], &other.px[(y * w + 100) * 4], 60 * 4);
  Feed(b, {rda_rect{100, 50, 60, 40}});
  Spin(0.1);
  EXPECT_LE(Worst(Shown(), CpuModel(b)), 1);
  // And again: the next surface had not had the first change yet.
  Pic c = b;
  for (int y = 120; y < 140; ++y) std::memcpy(&c.px[(y * w + 10) * 4], &other.px[(y * w + 10) * 4], 30 * 4);
  Feed(c, {rda_rect{10, 120, 30, 20}});
  Spin(0.1);
  EXPECT_LE(Worst(Shown(), CpuModel(c)), 1);
}

// A change too big for the model in one picture: MetalFX at once, the model
// once nothing has changed for 150 ms.
TEST_F(WindowUpscaleTest, BigChangesGoToMetalFXThenTheModel) {
  auto direct = PictureUpscaler::Create();
  ASSERT_TRUE(direct);
  if (!direct->HasMetalFX()) GTEST_SKIP() << "no MetalFX spatial scaler on this GPU";
  const int w = 1200, h = 800;  // far more than the model does in 4 ms
  Open(w, h);
  const Pic a = Make(w, h, 2);
  Feed(a, {});
  Spin(0.03);
  // MetalFX's own picture of it, for comparison.
  IOSurfaceRef src = nullptr, dst = nullptr;
  {
    src = MakeSurface(w, h);
    dst = MakeSurface(2 * w, 2 * h);
    IOSurfaceLock(src, 0, nullptr);
    auto* b = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(src));
    for (int y = 0; y < h; ++y) std::memcpy(b + y * IOSurfaceGetBytesPerRow(src), &a.px[static_cast<size_t>(y) * w * 4], w * 4);
    IOSurfaceUnlock(src, 0, nullptr);
    const std::vector<rda_rect> all{rda_rect{0, 0, w, h}};
    ASSERT_TRUE(direct->Run(Upscaler::kMetalFX, src, all, dst, all));
  }
  EXPECT_EQ(Worst(Shown(), Read(dst)), 0);
  Spin(0.8);  // the model goes over it
  EXPECT_LE(Worst(Shown(), CpuModel(a)), 1);
  CFRelease(src);
  CFRelease(dst);
}

// The menu: another way shows the current picture that way at once; pixels
// repeated shows the picture as it came.
TEST_F(WindowUpscaleTest, ChangingTheWayRepaintsAtOnce) {
  const int w = 240, h = 160;
  Open(w, h);
  const Pic a = Make(w, h, 3);
  Feed(a, {});
  Spin(0.1);
  ASSERT_LE(Worst(Shown(), CpuModel(a)), 1);
  SetUpscaler(Upscaler::kPixels);
  [NSNotificationCenter.defaultCenter postNotificationName:kUpscalerChanged object:nil];
  Spin(0.2);
  EXPECT_EQ(Worst(Shown(), a), 0);
  SetUpscaler(Upscaler::kModel);
  [NSNotificationCenter.defaultCenter postNotificationName:kUpscalerChanged object:nil];
  Spin(0.2);
  EXPECT_LE(Worst(Shown(), CpuModel(a)), 1);
}

// Not at exactly 2x (a window being resized): the picture as it came.
TEST_F(WindowUpscaleTest, OtherRatiosShowThePictureAsItCame) {
  const int w = 240, h = 160;
  Open(w, h);
  const Pic a = Make(w, h, 5);
  Feed(a, {});
  Spin(0.1);
  const CGFloat backing = win_.window.backingScaleFactor > 0 ? win_.window.backingScaleFactor : 1;
  [win_.window setContentSize:NSMakeSize(1.5 * w / backing, 1.5 * h / backing)];
  Spin(0.2);
  EXPECT_EQ(Worst(Shown(), a), 0);
  const Pic b = Make(w, h, 6);
  Feed(b, {});
  Spin(0.1);
  EXPECT_EQ(Worst(Shown(), b), 0);
}

}  // namespace
