// The text model that enlarges the 1x picture 2x for a Retina screen
// (docs/upscaler.md): its weights, trained by tools/upscaler/train.py, and the
// same computation on the CPU, which the tests hold the GPU kernel to.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rdmclient::upscaler {

// Each colour channel on its own: the 7 x 7 neighbourhood of a 1x pixel
// (edges repeated) gives the 2 x 2 pixels it becomes, in the order (0,0) (0,1)
// (1,0) (1,1): a linear filter plus a small network's correction. A
// neighbourhood all of one colour gives that colour (the network would change
// it by a rounding at most): most of a desktop, so most pixels cost nothing.
constexpr int kWindow = 7;
constexpr int kReach = 3;  // a 1x pixel changes the output this many pixels around it
constexpr int kTaps = kWindow * kWindow;
constexpr int kHidden1 = 16;
constexpr int kHidden2 = 8;

// Where each part starts in Weights(), in floats (upscaler_weights.inc).
constexpr size_t kLin = 0;
constexpr size_t kLinBias = kLin + kTaps * 4;
constexpr size_t kW1 = kLinBias + 4;
constexpr size_t kB1 = kW1 + kTaps * kHidden1;
constexpr size_t kW2 = kB1 + kHidden1;
constexpr size_t kB2 = kW2 + kHidden1 * kHidden2;
constexpr size_t kW3 = kB2 + kHidden2;
constexpr size_t kB3 = kW3 + kHidden2 * 4;
constexpr size_t kWeightCount = kB3 + 4;

const float* Weights();

// The model on a whole BGRA picture of w x h into dst (2w x 2h BGRA, opaque),
// computed on the CPU in single precision: the GPU kernel's sums in the same
// order, which in half precision come within two levels of 255 of these.
constexpr int kGpuTolerance = 2;
void UpscaleOnCpu(const uint8_t* src, int w, int h, size_t src_stride, uint8_t* dst, size_t dst_stride);

}  // namespace rdmclient::upscaler
