// Where a text field says the client's candidate window goes (docs/ime.md, 7.1).
#pragma once

#import <Foundation/Foundation.h>

#include <optional>

namespace rdm {

// `client` is the text field as InputMethodKit hands it to an input method
// (IMKTextInput). The start of the composition, or the caret when the field
// holds none: Cocoa screen coordinates, 1 point wide, the line's height.
// Nothing when the field gives no position.
//
// It asks for character 0, as every input method does: the index is counted
// from the start of the composition, or from the caret when there is none.
// InputMethodKit hands the index to the app as a byte offset (index x 2),
// and AppKit adds the field's markedRange, or its selectedRange when nothing
// is marked, before it asks the field for the rect
// (kEventTextInputOffsetToPos). An index past the composition gets nothing.
std::optional<NSRect> CompositionAnchor(id client);

}  // namespace rdm
