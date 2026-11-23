// Enlarges the 1x picture 2x on the GPU for a Retina screen (docs/upscaler.md):
// with the text model (upscaler_model.h) or with MetalFX's spatial scaler.
// Keeps the enlarged picture in a texture of its own, brings it up to date
// where the picture changed, and copies what a surface lacks into it.
#pragma once

#include <IOSurface/IOSurface.h>
#include <rdaccel/rdaccel.h>

#include <memory>
#include <vector>

namespace rdmclient {

// View > 1 倍画面放大: how a 1x picture shown at exactly 2x is enlarged.
enum class Upscaler { kPixels = 0, kMetalFX = 1, kModel = 2 };

class PictureUpscaler {
 public:
  // nullptr when this Mac has no Metal device or the kernels do not build.
  static std::unique_ptr<PictureUpscaler> Create();
  ~PictureUpscaler();

  bool HasMetalFX() const;

  // Brings the enlarged picture up to date from `src` (the whole current
  // picture, w x h BGRA) where it changed: `rects` (1x pixels) and kReach
  // around them are redone with `how` (kMetalFX enlarges the whole picture and
  // takes only those parts of it, so the rest keeps what made it). Then copies
  // `copy` (1x rects) of it into `dst` (2w x 2h BGRA) and waits for the GPU.
  // A new size starts the enlarged picture over: redo all of it then.
  bool Run(Upscaler how, IOSurfaceRef src, const std::vector<rda_rect>& rects, IOSurfaceRef dst,
           const std::vector<rda_rect>& copy);

  // Lets go of the textures and the surfaces they hold (the session ended).
  void Reset();

 private:
  PictureUpscaler();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdmclient
