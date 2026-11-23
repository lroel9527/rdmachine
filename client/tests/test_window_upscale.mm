// The window's side of the upscaler (docs/upscaler.md): frames given to a real
// RemoteWindowController, as rdaccel's decode thread gives them, and what its
// layer ends up showing. The window is never put on screen and nothing
// connects anywhere.
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <Metal/Metal.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
// differs between seeds somewhere. `plain`: on one background colour, as a
// desktop's text is; otherwise on a background that varies (as a photo or a
// video does), where every pixel costs the model.
Pic Make(int w, int h, int seed, bool plain = false) {
  Pic p{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const bool ink = ((x + seed) % 7 < 2 && (y / 9) % 2 == 0) || ((y + seed) % 11 == 3 && x % 13 < 9);
      const bool blank = plain && ((y / 9) % 4 == 3 || (x / 40) % 6 == 5);  // gaps between lines and columns
      const uint8_t bg = plain ? 250 : static_cast<uint8_t>(250 - (x + y) % 5);
      const uint8_t v = ink && !blank ? static_cast<uint8_t>(30 + (x * 7 + y * 3 + seed) % 40) : bg;
      uint8_t* q = &p.px[(static_cast<size_t>(y) * w + x) * 4];
      q[0] = v;
      q[1] = static_cast<uint8_t>(ink && !blank && seed % 2 ? 90 : v);
      q[2] = v;
      q[3] = 255;
    }
  }
  return p;
}

// A screen of 13 pt text drawn by Core Text on white with a grey sidebar, as a
// document or a chat is: most 7 x 7 neighbourhoods are one colour. `scroll`
// moves the text up by that many pixels.
Pic Page(int w, int h, int scroll) {
  Pic p{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(p.px.data(), w, h, 8, static_cast<size_t>(w) * 4, cs,
                                           static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) |
                                               static_cast<uint32_t>(kCGBitmapByteOrder32Little));
  CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
  CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
  CGContextSetRGBFillColor(ctx, 0.95, 0.95, 0.97, 1);
  CGContextFillRect(ctx, CGRectMake(0, 0, 260, h));
  CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 13, CFSTR("zh-Hans"));
  const char* lines[] = {"远程桌面用一倍分辨率渲染时，客户端把每个像素放大成两行两列。",
                         "The quick brown fox jumps over the lazy dog. 0123456789",
                         "窗口、菜单、按钮和输入框里的文字都应该清楚。",
                         "Data saver keeps text sharp while wallpapers and icons get cheaper."};
  for (int i = 0, y = h - 30 + scroll % 20; y > 0; ++i, y -= 20) {
    CGColorRef fg = CGColorCreateSRGB(0.11, 0.11, 0.12, 1);
    CFStringRef keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    CFTypeRef vals[] = {font, fg};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, reinterpret_cast<const void**>(keys),
                                               reinterpret_cast<const void**>(vals), 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFStringRef str = CFStringCreateWithCString(nullptr, lines[(i + scroll / 20) % 4], kCFStringEncodingUTF8);
    CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);
    CTLineRef ln = CTLineCreateWithAttributedString(as);
    CGContextSetTextPosition(ctx, 290, y);
    CTLineDraw(ln, ctx);
    if (i % 3 == 0) {  // the sidebar's labels
      CGContextSetTextPosition(ctx, 24, y);
      CTLineDraw(ln, ctx);
    }
    CFRelease(ln);
    CFRelease(as);
    CFRelease(str);
    CFRelease(attrs);
    CGColorRelease(fg);
  }
  CFRelease(font);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  for (size_t i = 3; i < p.px.size(); i += 4) p.px[i] = 255;
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

// The model's pixels for `r` of `p` (1x pixels), from the CPU version: worked
// out on `r` and the kReach around it, which is all they depend on.
Pic CpuModelPart(const Pic& p, rda_rect r) {
  const int k = upscaler::kReach;
  const int x0 = std::max(0, r.x - k), y0 = std::max(0, r.y - k);
  const int x1 = std::min(p.w, r.x + r.w + k), y1 = std::min(p.h, r.y + r.h + k);
  Pic crop{x1 - x0, y1 - y0, std::vector<uint8_t>(static_cast<size_t>(x1 - x0) * (y1 - y0) * 4)};
  for (int y = y0; y < y1; ++y) std::memcpy(&crop.px[static_cast<size_t>(y - y0) * crop.w * 4], &p.px[(static_cast<size_t>(y) * p.w + x0) * 4], crop.w * 4);
  const Pic big = CpuModel(crop);
  Pic out{2 * r.w, 2 * r.h, std::vector<uint8_t>(static_cast<size_t>(r.w) * r.h * 16)};
  for (int y = 0; y < 2 * r.h; ++y) {
    std::memcpy(&out.px[static_cast<size_t>(y) * out.w * 4],
                &big.px[(static_cast<size_t>(y + 2 * (r.y - y0)) * big.w + 2 * (r.x - x0)) * 4], out.w * 4);
  }
  return out;
}

// `r` (1x pixels) of an enlarged picture.
Pic Part(const Pic& big, rda_rect r) {
  Pic out{2 * r.w, 2 * r.h, std::vector<uint8_t>(static_cast<size_t>(r.w) * r.h * 16)};
  for (int y = 0; y < 2 * r.h; ++y) {
    std::memcpy(&out.px[static_cast<size_t>(y) * out.w * 4], &big.px[(static_cast<size_t>(y + 2 * r.y) * big.w + 2 * r.x) * 4], out.w * 4);
  }
  return out;
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
  EXPECT_LE(Worst(Shown(), CpuModel(a)), upscaler::kGpuTolerance);
  // A small change: only it (and kReach around it) is redone, with no seam.
  Pic b = a;
  const Pic other = Make(w, h, 4);
  for (int y = 50; y < 90; ++y) std::memcpy(&b.px[(y * w + 100) * 4], &other.px[(y * w + 100) * 4], 60 * 4);
  Feed(b, {rda_rect{100, 50, 60, 40}});
  Spin(0.1);
  EXPECT_LE(Worst(Shown(), CpuModel(b)), upscaler::kGpuTolerance);
  // And again: the next surface had not had the first change yet.
  Pic c = b;
  for (int y = 120; y < 140; ++y) std::memcpy(&c.px[(y * w + 10) * 4], &other.px[(y * w + 10) * 4], 30 * 4);
  Feed(c, {rda_rect{10, 120, 30, 20}});
  Spin(0.1);
  EXPECT_LE(Worst(Shown(), CpuModel(c)), upscaler::kGpuTolerance);
}

// The whole picture changing (scrolling, a video, another app) is the model's
// at once, as any change is.
TEST_F(WindowUpscaleTest, BigChangesAreTheModelsToo) {
  const int w = 1200, h = 800;
  Open(w, h);
  Feed(Make(w, h, 2), {});
  Spin(0.05);
  const Pic b = Make(w, h, 3);
  Feed(b, {});
  Spin(0.05);
  const Pic shown = Shown();
  for (const rda_rect r : {rda_rect{0, 0, 160, 120}, rda_rect{500, 300, 200, 200}, rda_rect{1040, 680, 160, 120}}) {
    EXPECT_LE(Worst(Part(shown, r), CpuModelPart(b, r)), upscaler::kGpuTolerance) << r.x << "," << r.y;
  }
}

// The menu: another way shows the current picture that way at once; pixels
// repeated shows the picture as it came.
TEST_F(WindowUpscaleTest, ChangingTheWayRepaintsAtOnce) {
  const int w = 240, h = 160;
  Open(w, h);
  const Pic a = Make(w, h, 3);
  Feed(a, {});
  Spin(0.1);
  ASSERT_LE(Worst(Shown(), CpuModel(a)), upscaler::kGpuTolerance);
  SetUpscaler(Upscaler::kPixels);
  [NSNotificationCenter.defaultCenter postNotificationName:kUpscalerChanged object:nil];
  Spin(0.2);
  EXPECT_EQ(Worst(Shown(), a), 0);
  SetUpscaler(Upscaler::kModel);
  [NSNotificationCenter.defaultCenter postNotificationName:kUpscalerChanged object:nil];
  Spin(0.2);
  EXPECT_LE(Worst(Shown(), CpuModel(a)), upscaler::kGpuTolerance);
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

// Changes of every size leave the rest of the screen alone and are the
// model's at once: in 7.2.0 MetalFX stood in for bigger ones and redid the
// whole picture, so every line on the screen switched looks back and forth
// (lroel: the overlay's method kept flickering).
TEST_F(WindowUpscaleTest, OrdinaryChangesLeaveTheRestAlone) {
  const int w = 1728, h = 1117;
  Open(w, h);
  Pic pic = Make(w, h, 1);
  const Pic ink = Make(w, h, 4);
  Feed(pic, {});
  Spin(0.8);  // settled: all of it the model's
  const rda_rect far{1500, 1000, 200, 100};  // no change comes near it
  const Pic farModel = CpuModelPart(pic, far);
  const int sizes[][2] = {{24, 24}, {600, 20}, {300, 80}, {400, 200}, {24, 24}, {1000, 700}};
  for (int i = 0; i < 24; ++i) {
    const int* sz = sizes[i % 6];
    const rda_rect r{(i * 97) % (1300 - sz[0]), (i * 61) % (900 - sz[1]), sz[0], sz[1]};
    for (int y = r.y; y < r.y + r.h; ++y) std::memcpy(&pic.px[(y * w + r.x) * 4], &ink.px[(y * w + r.x) * 4], r.w * 4);
    Feed(pic, {r});
    Spin(0.03);
    const Pic shown = Shown();
    ASSERT_LE(Worst(Part(shown, far), farModel), upscaler::kGpuTolerance) << "after change " << i << " (" << r.w << "x" << r.h << ")";
    const rda_rect part{r.x, r.y, std::min(r.w, 200), std::min(r.h, 120)};  // of the change: the model's
    ASSERT_LE(Worst(Part(shown, part), CpuModelPart(pic, part)), upscaler::kGpuTolerance) << "change " << i << " (" << r.w << "x" << r.h << ")";
  }
}

// RDM_UPSCALER_TIMING=1: how long a picture takes on the decode thread, from
// rdaccel's callback until it goes to the main thread (copying it in and
// enlarging it): what the client adds before the picture can be on screen.
// A page of text (most of it background the model skips) and a picture
// where everything varies (a photo, a video: the worst case).
TEST_F(WindowUpscaleTest, Latency) {
  if (!std::getenv("RDM_UPSCALER_TIMING")) GTEST_SKIP() << "set RDM_UPSCALER_TIMING=1";
  const int w = 1728, h = 1117;  // a 16-inch MacBook Pro's screen at 1x
  Open(w, h);
  auto report = [](const char* what, std::vector<double> v) {
    std::sort(v.begin(), v.end());
    std::printf("  %-40s median %6.2f ms   max %6.2f ms\n", what, v[v.size() / 2], v.back());
  };
  const struct {
    Upscaler how;
    const char* name;
  } ways[] = {{Upscaler::kPixels, "pixels repeated"}, {Upscaler::kMetalFX, "MetalFX"}, {Upscaler::kModel, "text model"}};
  for (const bool text : {true, false}) {
    for (const auto& way : ways) {
      SetUpscaler(way.how);
      [NSNotificationCenter.defaultCenter postNotificationName:kUpscalerChanged object:nil];
      auto picture = [&](int n) { return text ? Page(w, h, n * 3) : Make(w, h, n); };  // scrolling by 3 px
      Pic pic = picture(1);
      const Pic ink = picture(7);
      Feed(pic, {});
      Spin(0.3);
      auto timed = [&](const std::vector<rda_rect>& changed, double gap) {
        const auto t0 = std::chrono::steady_clock::now();
        Feed(pic, changed);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Spin(gap);  // the main thread shows it meanwhile
        return ms;
      };
      std::vector<double> keys, half, whole;
      for (int i = 0; i < 40; ++i) {  // typing: a glyph's cell changes, 50 times a second
        const int x = 300 + (i * 97) % (w - 330), y = (i * 61) % (h - 24);
        for (int r = y; r < y + 24; ++r) std::memcpy(&pic.px[(r * w + x) * 4], &ink.px[(r * w + x) * 4], 24 * 4);
        keys.push_back(timed({rda_rect{x, y, 24, 24}}, 0.02));
      }
      for (int i = 0; i < 30; ++i) {  // scrolling a panel: half the screen, 60 times a second
        pic = picture(10 + i);
        half.push_back(timed({rda_rect{300, 80, 1100, 900}}, 0.016));
      }
      for (int i = 0; i < 30; ++i) {  // another app, a full-screen video: everything
        pic = picture(50 + i);
        whole.push_back(timed({}, 0.016));
      }
      std::printf("%s, %dx%d, %s:\n", way.name, w, h, text ? "a page of text" : "everything varying (photo, video)");
      report("keystroke (24 x 24 changed)", keys);
      report("scrolling (1100 x 900 changed)", half);
      report("whole picture changed", whole);
    }
  }
}

}  // namespace
