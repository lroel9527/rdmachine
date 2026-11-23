#include "composition_anchor.h"

#import <InputMethodKit/InputMethodKit.h>

namespace rdm {

std::optional<NSRect> CompositionAnchor(id client) {
  NSRect line = NSZeroRect;
  [client attributesForCharacterIndex:0 lineHeightRectangle:&line];
  if (line.size.height <= 0) return std::nullopt;
  line.size.width = 1;
  return line;
}

}  // namespace rdm
