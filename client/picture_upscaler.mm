#include "picture_upscaler.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>
#include <map>

#include "upscaler_model.h"

namespace rdmclient {

namespace {

// One thread per 1x pixel of a rect. model_upscale: the text model
// (upscaler_model.h), one colour channel at a time, in the order UpscaleOnCpu
// sums, in half precision with the weights compiled in (WTH) and every loop
// unrolled: three times faster than in single precision, and within two
// levels of 255 of UpscaleOnCpu. A neighbourhood of one colour is copied, as
// there. pixels_upscale: every pixel repeated as 2 x 2.
const char* kKernels = R"(
#include <metal_stdlib>
using namespace metal;

struct Region { uint2 origin; uint2 size; uint2 picture; };

static float4 channel(texture2d<float, access::read> src, int2 p, int2 lim, int c) {
  constant half* w = WTH;
  half x[TAPS];
  #pragma unroll
  for (int i = 0; i < TAPS; ++i) {
    x[i] = (half)src.read(uint2(clamp(p + int2(i % WINDOW - REACH, i / WINDOW - REACH), int2(0), lim)))[c];
  }
  half4 ph = half4(w[LIN_BIAS], w[LIN_BIAS + 1], w[LIN_BIAS + 2], w[LIN_BIAS + 3]);
  #pragma unroll
  for (int i = 0; i < TAPS; ++i) {
    ph += x[i] * half4(w[LIN + i * 4], w[LIN + i * 4 + 1], w[LIN + i * 4 + 2], w[LIN + i * 4 + 3]);
  }
  #pragma unroll
  for (int i = 0; i < TAPS; ++i) x[i] -= 0.5h;
  half h2[H2];
  #pragma unroll
  for (int m = 0; m < H2; ++m) h2[m] = w[B2 + m];
  #pragma unroll
  for (int j = 0; j < H1; ++j) {
    half s = w[B1 + j];
    #pragma unroll
    for (int i = 0; i < TAPS; ++i) s += x[i] * w[W1 + i * H1 + j];
    s = max(s, 0.0h);
    #pragma unroll
    for (int m = 0; m < H2; ++m) h2[m] += s * w[W2 + j * H2 + m];
  }
  half4 o = half4(w[B3], w[B3 + 1], w[B3 + 2], w[B3 + 3]);
  #pragma unroll
  for (int m = 0; m < H2; ++m) {
    o += max(h2[m], 0.0h) * half4(w[W3 + m * 4], w[W3 + m * 4 + 1], w[W3 + m * 4 + 2], w[W3 + m * 4 + 3]);
  }
  return float4(clamp(ph + o, 0.0h, 1.0h));  // the 4 phases of this channel
}

kernel void model_upscale(texture2d<float, access::read> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          constant Region& r [[buffer(1)]],
                          uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= r.size.x || gid.y >= r.size.y) return;
  const int2 p = int2(r.origin + gid);
  const int2 lim = int2(r.picture) - 1;
  const uint2 q = uint2(p) * 2;
  // A neighbourhood all of one colour (most of a desktop: backgrounds) stays
  // that colour; the model would change it by a rounding at most.
  const float4 c = float4(src.read(uint2(p)).rgb, 1.0f);
  bool flat = true;
  for (int i = 0; i < TAPS && flat; ++i) {
    flat = all(src.read(uint2(clamp(p + int2(i % WINDOW - REACH, i / WINDOW - REACH), int2(0), lim))).rgb == c.rgb);
  }
  if (flat) {
    dst.write(c, q);
    dst.write(c, q + uint2(1, 0));
    dst.write(c, q + uint2(0, 1));
    dst.write(c, q + uint2(1, 1));
    return;
  }
  const float4 cr = channel(src, p, lim, 0);
  const float4 cg = channel(src, p, lim, 1);
  const float4 cb = channel(src, p, lim, 2);
  dst.write(float4(cr[0], cg[0], cb[0], 1.0f), q);
  dst.write(float4(cr[1], cg[1], cb[1], 1.0f), q + uint2(1, 0));
  dst.write(float4(cr[2], cg[2], cb[2], 1.0f), q + uint2(0, 1));
  dst.write(float4(cr[3], cg[3], cb[3], 1.0f), q + uint2(1, 1));
}

kernel void pixels_upscale(texture2d<float, access::read> src [[texture(0)]],
                           texture2d<float, access::write> dst [[texture(1)]],
                           constant Region& r [[buffer(1)]],
                           uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= r.size.x || gid.y >= r.size.y) return;
  const uint2 p = r.origin + gid;
  const float4 c = float4(src.read(p).rgb, 1.0f);
  const uint2 q = p * 2;
  dst.write(c, q);
  dst.write(c, q + uint2(1, 0));
  dst.write(c, q + uint2(0, 1));
  dst.write(c, q + uint2(1, 1));
}
)";

struct Region {  // the kernels' Region: three uint2
  uint32_t x, y, w, h, pw, ph;
};

// `r` within a w x h picture, grown by `grow` on each side; empty if outside.
rda_rect Clip(const rda_rect& r, int32_t grow, int32_t w, int32_t h) {
  const int32_t x0 = std::clamp(r.x - grow, 0, w), y0 = std::clamp(r.y - grow, 0, h);
  const int32_t x1 = std::clamp(r.x + r.w + grow, 0, w), y1 = std::clamp(r.y + r.h + grow, 0, h);
  return rda_rect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

}  // namespace

struct PictureUpscaler::Impl {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLComputePipelineState> model, pixels;
  bool fx_supported = false;
  int32_t w = 0, h = 0;
  id<MTLTexture> master;  // the enlarged picture, 2w x 2h
  id<MTLFXSpatialScaler> fx;
  id<MTLTexture> fxOut;   // MetalFX's whole picture, which only what changed is taken from
  std::map<IOSurfaceRef, id<MTLTexture>> textures;  // the window's surfaces, as textures

  id<MTLTexture> TextureFor(IOSurfaceRef s) {
    auto it = textures.find(s);
    if (it != textures.end()) return it->second;
    if (textures.size() >= 8) textures.clear();  // surfaces of an earlier size
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:IOSurfaceGetWidth(s)
                                                          height:IOSurfaceGetHeight(s)
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | (fx ? fx.colorTextureUsage : 0);
    td.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    id<MTLTexture> t = [device newTextureWithDescriptor:td iosurface:s plane:0];
    if (t) textures[s] = t;
    return t;
  }

  // A picture of a new size: the enlarged picture and the scaler start over.
  bool Resize(int32_t nw, int32_t nh) {
    if (nw == w && nh == h && master) return false;
    textures.clear();
    fx = nil;
    fxOut = nil;
    if (fx_supported) {
      MTLFXSpatialScalerDescriptor* d = [[MTLFXSpatialScalerDescriptor alloc] init];
      d.inputWidth = nw;
      d.inputHeight = nh;
      d.outputWidth = 2 * nw;
      d.outputHeight = 2 * nh;
      d.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
      d.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
      d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;  // sRGB values, as the picture is
      fx = [d newSpatialScalerWithDevice:device];
    }
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:2 * nw
                                                                                 height:2 * nh
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModePrivate;
    master = [device newTextureWithDescriptor:td];
    if (fx) {
      td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | fx.outputTextureUsage;
      fxOut = [device newTextureWithDescriptor:td];  // private: the only kind MetalFX writes
      if (!fxOut) fx = nil;
    }
    w = nw;
    h = nh;
    return true;
  }
};

PictureUpscaler::PictureUpscaler() : impl_(std::make_unique<Impl>()) {}
PictureUpscaler::~PictureUpscaler() = default;

std::unique_ptr<PictureUpscaler> PictureUpscaler::Create() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) return nullptr;
  using namespace upscaler;
  NSString* defs = [NSString stringWithFormat:@"#define WINDOW %d\n#define TAPS %d\n#define REACH %d\n#define H1 %d\n#define H2 %d\n"
                                              @"#define LIN %zu\n#define LIN_BIAS %zu\n#define W1 %zu\n#define B1 %zu\n"
                                              @"#define W2 %zu\n#define B2 %zu\n#define W3 %zu\n#define B3 %zu\n",
                                              kWindow, kTaps, kReach, kHidden1, kHidden2, kLin, kLinBias, kW1, kB1, kW2, kB2,
                                              kW3, kB3];
  // The weights go into the program as constants: loading each from a buffer
  // as it is multiplied made the network 1.6 times slower.
  NSMutableString* table = [NSMutableString stringWithString:@"constant half WTH[] = {"];
  for (size_t i = 0; i < kWeightCount; ++i) [table appendFormat:@"%s%.9g", i ? "," : "", Weights()[i]];
  [table appendString:@"};\n"];
  defs = [defs stringByAppendingString:table];
  MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
  // The same sums in the same order as UpscaleOnCpu (the tests hold it to two levels of it).
  if (@available(macOS 15.0, *)) {
    opts.mathMode = MTLMathModeSafe;
  } else {
    opts.fastMathEnabled = NO;
  }
  NSError* error = nil;
  id<MTLLibrary> lib = [device newLibraryWithSource:[defs stringByAppendingString:@(kKernels)] options:opts error:&error];
  if (!lib) {
    NSLog(@"upscaler: kernels did not build: %@", error);
    return nullptr;
  }
  std::unique_ptr<PictureUpscaler> u(new PictureUpscaler());
  Impl& m = *u->impl_;
  m.device = device;
  m.queue = [device newCommandQueue];
  m.model = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"model_upscale"] error:&error];
  m.pixels = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"pixels_upscale"] error:&error];
  if (!m.queue || !m.model || !m.pixels) {
    NSLog(@"upscaler: no pipeline: %@", error);
    return nullptr;
  }
  m.fx_supported = [MTLFXSpatialScalerDescriptor supportsDevice:device];
  return u;
}

bool PictureUpscaler::HasMetalFX() const { return impl_->fx_supported; }

bool PictureUpscaler::Run(Upscaler how, IOSurfaceRef src, const std::vector<rda_rect>& rects, IOSurfaceRef dst,
                          const std::vector<rda_rect>& copy) {
  Impl& m = *impl_;
  const auto w = static_cast<int32_t>(IOSurfaceGetWidth(src)), h = static_cast<int32_t>(IOSurfaceGetHeight(src));
  if (w <= 0 || h <= 0 || static_cast<int32_t>(IOSurfaceGetWidth(dst)) != 2 * w ||
      static_cast<int32_t>(IOSurfaceGetHeight(dst)) != 2 * h) {
    return false;
  }
  const std::vector<rda_rect> whole{rda_rect{0, 0, w, h}};
  const bool fresh = m.Resize(w, h);
  if (!m.master) return false;
  const std::vector<rda_rect>* redo = fresh ? &whole : &rects;
  const std::vector<rda_rect>& give = fresh ? whole : copy;
  id<MTLTexture> in = m.TextureFor(src), out = m.TextureFor(dst);
  if (!in || !out) return false;
  id<MTLCommandBuffer> cb = [m.queue commandBuffer];
  if (how == Upscaler::kMetalFX && !m.fx) how = Upscaler::kPixels;  // the scaler did not build for this size
  // Every way redoes `rects` and kReach around them: the model's pixels there
  // were made from what was there before.
  const int32_t reach = upscaler::kReach;
  id<MTLBlitCommandEncoder> blit = nil;
  if (how == Upscaler::kMetalFX) {
    m.fx.colorTexture = in;
    m.fx.outputTexture = m.fxOut;
    m.fx.inputContentWidth = w;
    m.fx.inputContentHeight = h;
    [m.fx encodeToCommandBuffer:cb];
    blit = [cb blitCommandEncoder];
    for (const auto& r0 : *redo) {
      const rda_rect r = Clip(r0, reach, w, h);
      if (r.w <= 0 || r.h <= 0) continue;
      [blit copyFromTexture:m.fxOut
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(2 * r.x, 2 * r.y, 0)
                 sourceSize:MTLSizeMake(2 * r.w, 2 * r.h, 1)
                  toTexture:m.master
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(2 * r.x, 2 * r.y, 0)];
    }
  } else {
    const bool model = how == Upscaler::kModel;
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:model ? m.model : m.pixels];
    [enc setTexture:in atIndex:0];
    [enc setTexture:m.master atIndex:1];
    for (const auto& r0 : *redo) {
      const rda_rect r = Clip(r0, reach, w, h);
      if (r.w <= 0 || r.h <= 0) continue;
      const Region reg{static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y), static_cast<uint32_t>(r.w),
                       static_cast<uint32_t>(r.h), static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
      [enc setBytes:&reg length:sizeof reg atIndex:1];
      [enc dispatchThreads:MTLSizeMake(r.w, r.h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    }
    [enc endEncoding];
  }
  if (!blit) blit = [cb blitCommandEncoder];
  for (const auto& r0 : give) {
    const rda_rect r = Clip(r0, 0, w, h);
    if (r.w <= 0 || r.h <= 0) continue;
    [blit copyFromTexture:m.master
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(2 * r.x, 2 * r.y, 0)
               sourceSize:MTLSizeMake(2 * r.w, 2 * r.h, 1)
                toTexture:out
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(2 * r.x, 2 * r.y, 0)];
  }
  [blit endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  if (cb.status != MTLCommandBufferStatusCompleted) {
    NSLog(@"upscaler: GPU work failed: %@", cb.error);
    return false;
  }
  return true;
}

void PictureUpscaler::Reset() {
  impl_->textures.clear();
  impl_->master = nil;
  impl_->fx = nil;
  impl_->fxOut = nil;
  impl_->w = impl_->h = 0;
}

}  // namespace rdmclient
