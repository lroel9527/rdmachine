#include "upscaler_model.h"

#include <algorithm>
#include <cmath>

namespace rdmclient::upscaler {

namespace {

const float kData[] = {
#include "upscaler_weights.inc"
};
static_assert(sizeof(kData) / sizeof(kData[0]) == kWeightCount, "upscaler_weights.inc does not fit the model's layout");

}  // namespace

const float* Weights() { return kData; }

void UpscaleOnCpu(const uint8_t* src, int w, int h, size_t src_stride, uint8_t* dst, size_t dst_stride) {
  const float* wt = kData;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t* centre = src + static_cast<size_t>(y) * src_stride + static_cast<size_t>(x) * 4;
      bool flat = true;
      for (int dy = -kReach; dy <= kReach && flat; ++dy) {
        const int yy = std::clamp(y + dy, 0, h - 1);
        for (int dx = -kReach; dx <= kReach && flat; ++dx) {
          const uint8_t* q = src + static_cast<size_t>(yy) * src_stride + static_cast<size_t>(std::clamp(x + dx, 0, w - 1)) * 4;
          flat = q[0] == centre[0] && q[1] == centre[1] && q[2] == centre[2];
        }
      }
      if (flat) {
        for (int k = 0; k < 4; ++k) {
          uint8_t* o = dst + static_cast<size_t>(2 * y + k / 2) * dst_stride + static_cast<size_t>(2 * x + k % 2) * 4;
          o[0] = centre[0];
          o[1] = centre[1];
          o[2] = centre[2];
          o[3] = 255;
        }
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        float in[kTaps];
        for (int dy = -kReach, i = 0; dy <= kReach; ++dy) {
          const int yy = std::clamp(y + dy, 0, h - 1);
          for (int dx = -kReach; dx <= kReach; ++dx, ++i) {
            const int xx = std::clamp(x + dx, 0, w - 1);
            in[i] = src[static_cast<size_t>(yy) * src_stride + static_cast<size_t>(xx) * 4 + c] / 255.0f;
          }
        }
        float ph[4];
        for (int k = 0; k < 4; ++k) ph[k] = wt[kLinBias + k];
        for (int i = 0; i < kTaps; ++i) {
          for (int k = 0; k < 4; ++k) ph[k] += in[i] * wt[kLin + i * 4 + k];
        }
        float h2[kHidden2];
        for (int m = 0; m < kHidden2; ++m) h2[m] = wt[kB2 + m];
        for (int j = 0; j < kHidden1; ++j) {
          float s = wt[kB1 + j];
          for (int i = 0; i < kTaps; ++i) s += (in[i] - 0.5f) * wt[kW1 + i * kHidden1 + j];
          s = std::max(s, 0.0f);
          for (int m = 0; m < kHidden2; ++m) h2[m] += s * wt[kW2 + j * kHidden2 + m];
        }
        float o[4];
        for (int k = 0; k < 4; ++k) o[k] = wt[kB3 + k];
        for (int m = 0; m < kHidden2; ++m) {
          const float a = std::max(h2[m], 0.0f);
          for (int k = 0; k < 4; ++k) o[k] += a * wt[kW3 + m * 4 + k];
        }
        for (int k = 0; k < 4; ++k) {
          const float v = std::clamp(ph[k] + o[k], 0.0f, 1.0f);
          const size_t oy = static_cast<size_t>(2 * y + k / 2), ox = static_cast<size_t>(2 * x + k % 2);
          dst[oy * dst_stride + ox * 4 + c] = static_cast<uint8_t>(std::lround(v * 255.0f));
        }
      }
      for (int k = 0; k < 4; ++k) {
        dst[static_cast<size_t>(2 * y + k / 2) * dst_stride + static_cast<size_t>(2 * x + k % 2) * 4 + 3] = 255;
      }
    }
  }
}

}  // namespace rdmclient::upscaler
