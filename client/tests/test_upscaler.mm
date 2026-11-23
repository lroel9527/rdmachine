// The 1x -> 2x upscaler (docs/upscaler.md) on text drawn by Core Text, as
// macOS draws a window at 1x and at 2x.
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#import <Metal/Metal.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "picture_upscaler.h"
#include "upscaler_model.h"

using rdmclient::PictureUpscaler;
using rdmclient::Upscaler;

namespace {

IOSurfaceRef MakeSurface(int w, int h) {
  const int bpe = 4;
  CFMutableDictionaryRef d = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
  const uint32_t fmt = 0x42475241u;  // 'BGRA'
  auto put = [&](CFStringRef k, const void* v, CFNumberType t) {
    CFNumberRef n = CFNumberCreate(nullptr, t, v);
    CFDictionarySetValue(d, k, n);
    CFRelease(n);
  };
  put(kIOSurfaceWidth, &w, kCFNumberIntType);
  put(kIOSurfaceHeight, &h, kCFNumberIntType);
  put(kIOSurfaceBytesPerElement, &bpe, kCFNumberIntType);
  put(kIOSurfacePixelFormat, &fmt, kCFNumberSInt32Type);
  IOSurfaceRef s = IOSurfaceCreate(d);
  CFRelease(d);
  return s;
}

// A BGRA picture (premultiplied, opaque), as the client's surfaces hold it.
struct Picture {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  size_t stride() const { return static_cast<size_t>(w) * 4; }
};

// Text and coloured bands drawn at `scale`, the same layout in points at any
// scale, with the settings AppKit uses for a window.
Picture Draw(int wpt, int hpt, int scale, const char* line2 = "窗口、菜单和按钮里的文字 0123456789") {
  Picture p;
  p.w = wpt * scale;
  p.h = hpt * scale;
  p.px.assign(p.stride() * p.h, 0);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(p.px.data(), p.w, p.h, 8, p.stride(), cs,
                                           static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
  CGContextSetShouldSmoothFonts(ctx, true);
  CGContextSetShouldSubpixelPositionFonts(ctx, true);
  CGContextSetShouldSubpixelQuantizeFonts(ctx, true);
  CGContextScaleCTM(ctx, scale, scale);
  const CGFloat bands[][6] = {{1, 1, 1, 0.11, 0.11, 0.12}, {0.12, 0.12, 0.12, 0.9, 0.9, 0.92},
                              {1, 1, 1, 1, 0.23, 0.19}, {0.04, 0.52, 1, 1, 1, 1}};
  const char* lines[] = {"The quick brown fox jumps over the lazy dog.", line2, "远程桌面 Retina 文字清晰度",
                         "连接 Connect"};
  const CGFloat band = hpt / 4.0;
  for (int i = 0; i < 4; ++i) {
    const CGFloat* b = bands[i];
    CGContextSetRGBFillColor(ctx, b[0], b[1], b[2], 1);
    CGContextFillRect(ctx, CGRectMake(0, hpt - (i + 1) * band, wpt, band));
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 13, CFSTR("zh-Hans"));
    CGColorRef fg = CGColorCreateSRGB(b[3], b[4], b[5], 1);
    CFStringRef keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    CFTypeRef vals[] = {font, fg};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, reinterpret_cast<const void**>(keys),
                                               reinterpret_cast<const void**>(vals), 2,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef str = CFStringCreateWithCString(nullptr, lines[i], kCFStringEncodingUTF8);
    CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);
    CTLineRef ln = CTLineCreateWithAttributedString(as);
    CGContextSetTextPosition(ctx, 6.25, std::round(hpt - (i + 1) * band + 6));
    CTLineDraw(ln, ctx);
    CFRelease(ln);
    CFRelease(as);
    CFRelease(str);
    CFRelease(attrs);
    CGColorRelease(fg);
    CFRelease(font);
  }
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return p;
}

void Put(IOSurfaceRef s, const Picture& p) {
  IOSurfaceLock(s, 0, nullptr);
  auto* dst = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(s));
  const size_t ds = IOSurfaceGetBytesPerRow(s);
  for (int y = 0; y < p.h; ++y) std::memcpy(dst + y * ds, p.px.data() + y * p.stride(), p.stride());
  IOSurfaceUnlock(s, 0, nullptr);
}

Picture Get(IOSurfaceRef s) {
  Picture p;
  p.w = static_cast<int>(IOSurfaceGetWidth(s));
  p.h = static_cast<int>(IOSurfaceGetHeight(s));
  p.px.resize(p.stride() * p.h);
  IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
  const auto* src = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(s));
  const size_t ss = IOSurfaceGetBytesPerRow(s);
  for (int y = 0; y < p.h; ++y) std::memcpy(p.px.data() + y * p.stride(), src + y * ss, p.stride());
  IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
  return p;
}

// PSNR of the colour channels, in dB.
double Psnr(const Picture& a, const Picture& b) {
  double se = 0;
  size_t n = 0;
  for (size_t i = 0; i < a.px.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      const double d = a.px[i + c] - b.px[i + c];
      se += d * d;
      ++n;
    }
  }
  return 10 * std::log10(255.0 * 255.0 / std::max(se / n, 1e-9));
}

Picture Repeat(const Picture& p) {
  Picture o;
  o.w = p.w * 2;
  o.h = p.h * 2;
  o.px.resize(o.stride() * o.h);
  for (int y = 0; y < o.h; ++y) {
    for (int x = 0; x < o.w; ++x) std::memcpy(&o.px[y * o.stride() + x * 4], &p.px[(y / 2) * p.stride() + (x / 2) * 4], 4);
  }
  for (size_t i = 3; i < o.px.size(); i += 4) o.px[i] = 255;
  return o;
}

class UpscalerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!MTLCreateSystemDefaultDevice()) GTEST_SKIP() << "no Metal device";
    up_ = PictureUpscaler::Create();
    ASSERT_TRUE(up_) << "the kernels did not build (the log says why)";
  }
  void TearDown() override {
    for (IOSurfaceRef s : surfaces_) CFRelease(s);
  }
  IOSurfaceRef Surface(int w, int h) {
    surfaces_.push_back(MakeSurface(w, h));
    return surfaces_.back();
  }
  // The picture enlarged from scratch with `how`.
  Picture Enlarge(Upscaler how, const Picture& p) {
    IOSurfaceRef src = Surface(p.w, p.h), dst = Surface(2 * p.w, 2 * p.h);
    Put(src, p);
    const std::vector<rda_rect> all{rda_rect{0, 0, p.w, p.h}};
    EXPECT_TRUE(up_->Run(how, src, all, dst, all));
    return Get(dst);
  }
  std::unique_ptr<PictureUpscaler> up_;
  std::vector<IOSurfaceRef> surfaces_;
};

TEST_F(UpscalerTest, ModelOnTheGpuIsTheModelOnTheCpu) {
  const Picture p = Draw(240, 80, 1);
  const Picture gpu = Enlarge(Upscaler::kModel, p);
  Picture cpu = gpu;
  rdmclient::upscaler::UpscaleOnCpu(p.px.data(), p.w, p.h, p.stride(), cpu.px.data(), cpu.stride());
  int worst = 0;
  size_t off = 0;
  for (size_t i = 0; i < gpu.px.size(); ++i) {
    const int d = std::abs(gpu.px[i] - cpu.px[i]);
    worst = std::max(worst, d);
    if (d) ++off;
  }
  EXPECT_LE(worst, 1);                       // rounding only
  EXPECT_LT(off, gpu.px.size() / 100);       // and rarely
}

// Only what changed is redone, kReach around it: the same pixels as
// enlarging the changed picture from scratch, with no seams.
TEST_F(UpscalerTest, RedoingChangedRectsMatchesStartingOver) {
  const Picture a = Draw(240, 80, 1);
  const Picture b = Draw(240, 80, 1, "改过的一行：Changed line 9876543210");
  std::vector<rda_rect> changed;
  int x0 = a.w, y0 = a.h, x1 = 0, y1 = 0;
  for (int y = 0; y < a.h; ++y) {
    for (int x = 0; x < a.w; ++x) {
      if (std::memcmp(&a.px[y * a.stride() + x * 4], &b.px[y * b.stride() + x * 4], 4)) {
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1);
      }
    }
  }
  ASSERT_LT(x0, x1);
  changed.push_back(rda_rect{x0, y0, x1 - x0, y1 - y0});
  IOSurfaceRef src = Surface(a.w, a.h), dst = Surface(2 * a.w, 2 * a.h);
  const std::vector<rda_rect> all{rda_rect{0, 0, a.w, a.h}};
  Put(src, a);
  ASSERT_TRUE(up_->Run(Upscaler::kModel, src, all, dst, all));
  Put(src, b);
  ASSERT_TRUE(up_->Run(Upscaler::kModel, src, changed, dst, all));
  const Picture redone = Get(dst);
  PictureUpscaler::Create().swap(up_);  // a fresh one: starts over
  const Picture fresh = Enlarge(Upscaler::kModel, b);
  EXPECT_EQ(redone.px, fresh.px);
}

// The weights are wired right (phases, channels, edges): on text the model is
// far closer to the native 2x drawing than repeating pixels.
TEST_F(UpscalerTest, ModelIsCloserToRetinaTextThanRepeatedPixels) {
  const Picture lo = Draw(240, 80, 1), hi = Draw(240, 80, 2);
  const double pixels = Psnr(Repeat(lo), hi), model = Psnr(Enlarge(Upscaler::kModel, lo), hi);
  std::printf("PSNR against native 2x: pixels repeated %.2f dB, model %.2f dB\n", pixels, model);
  EXPECT_GT(model, pixels + 1.5);
}

TEST_F(UpscalerTest, MetalFXIsCloserToRetinaTextThanRepeatedPixels) {
  if (!up_->HasMetalFX()) GTEST_SKIP() << "no MetalFX spatial scaler on this GPU";
  const Picture lo = Draw(240, 80, 1), hi = Draw(240, 80, 2);
  const double pixels = Psnr(Repeat(lo), hi), fx = Psnr(Enlarge(Upscaler::kMetalFX, lo), hi);
  std::printf("PSNR against native 2x: pixels repeated %.2f dB, MetalFX %.2f dB\n", pixels, fx);
  EXPECT_GT(fx, pixels + 0.5);
}

TEST_F(UpscalerTest, PixelsAreRepeated) {
  const Picture p = Draw(120, 40, 1);
  EXPECT_EQ(Enlarge(Upscaler::kPixels, p).px, Repeat(p).px);
}

// A picture of another size starts the enlarged picture over, whatever the
// caller says changed.
TEST_F(UpscalerTest, ANewSizeIsDoneWhole) {
  (void)Enlarge(Upscaler::kModel, Draw(120, 40, 1));
  const Picture p = Draw(160, 48, 1);
  IOSurfaceRef src = Surface(p.w, p.h), dst = Surface(2 * p.w, 2 * p.h);
  Put(src, p);
  ASSERT_TRUE(up_->Run(Upscaler::kModel, src, {}, dst, {}));
  Picture cpu = Get(dst);
  const Picture gpu = cpu;
  rdmclient::upscaler::UpscaleOnCpu(p.px.data(), p.w, p.h, p.stride(), cpu.px.data(), cpu.stride());
  int worst = 0;
  for (size_t i = 0; i < gpu.px.size(); ++i) worst = std::max(worst, std::abs(gpu.px[i] - cpu.px[i]));
  EXPECT_LE(worst, 1);
}

// RDM_UPSCALER_TIMING=1: GPU time for whole pictures of common sizes.
TEST_F(UpscalerTest, Timing) {
  if (!std::getenv("RDM_UPSCALER_TIMING")) GTEST_SKIP() << "set RDM_UPSCALER_TIMING=1";
  const int sizes[][2] = {{400, 200}, {1512, 982}, {1728, 1117}, {2560, 1440}};
  for (const auto& s : sizes) {
    IOSurfaceRef src = Surface(s[0], s[1]), dst = Surface(2 * s[0], 2 * s[1]);
    const std::vector<rda_rect> all{rda_rect{0, 0, s[0], s[1]}};
    for (Upscaler how : {Upscaler::kModel, Upscaler::kMetalFX}) {
      if (how == Upscaler::kMetalFX && !up_->HasMetalFX()) continue;
      double best = 1e9;
      for (int i = 0; i < 8; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        ASSERT_TRUE(up_->Run(how, src, all, dst, all));
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      }
      std::printf("%dx%d %s: %.2f ms\n", s[0], s[1], how == Upscaler::kModel ? "model" : "MetalFX", best);
    }
  }
}

}  // namespace
