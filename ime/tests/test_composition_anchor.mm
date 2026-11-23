// RDMachine Input asks a text field where the client's candidate window goes
// (docs/ime.md, 7.1). The index it gives goes to the app as a byte offset,
// and AppKit counts it from the field's composition, or from its caret when
// it holds none. These tests send the query through AppKit's own handling of
// that offset (NSTextInputContext), to a field that, like Terminal, gives
// nothing for a position past its text.
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#include <gtest/gtest.h>
#include <objc/message.h>

#include "composition_anchor.h"

static const NSRange kNotAsked = {NSNotFound, 0};

// A text field of `length` characters, each 10 points wide on one line.
@interface Field : NSView <NSTextInputClient>
@property NSUInteger length;
@property NSRange marked, selected;
@property NSRange asked;  // the range AppKit last asked for
@property NSRect given;   // what the field answered
@end

@implementation Field
- (NSRange)markedRange {
  return _marked;
}
- (NSRange)selectedRange {
  return _selected;
}
- (BOOL)hasMarkedText {
  return _marked.location != NSNotFound && _marked.length > 0;
}
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)actual {
  _asked = r;
  // Past the text there is nothing (Terminal does not fold it back).
  _given = r.location > _length ? NSZeroRect : NSMakeRect(100 + 10.0 * r.location, 300, 10.0 * r.length, 15);
  if (actual) *actual = r;
  return _given;
}
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)actual {
  if (r.location > _length) return nil;
  if (actual) *actual = r;
  return [[NSAttributedString alloc] initWithString:[@"" stringByPaddingToLength:r.length
                                                                       withString:@"a"
                                                                  startingAtIndex:0]];
}
- (void)insertText:(id)s replacementRange:(NSRange)r {
}
- (void)setMarkedText:(id)s selectedRange:(NSRange)sel replacementRange:(NSRange)r {
}
- (void)unmarkText {
}
- (NSArray*)validAttributesForMarkedText {
  return @[];
}
- (NSUInteger)characterIndexForPoint:(NSPoint)p {
  return NSNotFound;
}
- (void)doCommandBySelector:(SEL)s {
}
@end

// The text field as an input method sees it: InputMethodKit's side in the app
// turns attributesForCharacterIndex:lineHeightRectangle: into a
// kEventTextInputOffsetToPos event with the index as a byte offset, and the
// app's text input context answers it from the field (macOS 27,
// -[IMKInputSession_Modern _createAndSendOffsetToPointEvent:completionHandler:]).
// Its line rect here is the field's own answer.
@interface InputMethodClient : NSObject
- (instancetype)initWithField:(Field*)field;
- (NSDictionary*)attributesForCharacterIndex:(NSUInteger)index lineHeightRectangle:(NSRect*)line;
@end

@implementation InputMethodClient {
  Field* _field;
  NSTextInputContext* _context;
}
- (instancetype)initWithField:(Field*)field {
  if ((self = [super init])) {
    _field = field;
    _context = [[NSTextInputContext alloc] initWithClient:field];
  }
  return self;
}
- (BOOL)canSend {
  return [_context respondsToSelector:NSSelectorFromString(@"handleTSMEvent:")];
}
- (NSDictionary*)attributesForCharacterIndex:(NSUInteger)index lineHeightRectangle:(NSRect*)line {
  _field.asked = kNotAsked;
  EventRef event = nullptr;
  CreateEvent(nullptr, kEventClassTextInput, kEventTextInputOffsetToPos, 0, kEventAttributeNone, &event);
  const SInt64 offset = static_cast<SInt64>(index) * 2;
  SetEventParameter(event, kEventParamTextInputSendTextOffset, typeSInt64, sizeof offset, &offset);
  const int status = reinterpret_cast<int (*)(id, SEL, EventRef)>(objc_msgSend)(
      _context, NSSelectorFromString(@"handleTSMEvent:"), event);
  ReleaseEvent(event);
  if (line) *line = status == noErr && _field.asked.location != NSNotFound ? _field.given : NSZeroRect;
  return @{};
}
@end

namespace {

class CompositionAnchorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    [NSApplication sharedApplication];
    field_ = [[Field alloc] initWithFrame:NSMakeRect(0, 0, 400, 100)];
    client_ = [[InputMethodClient alloc] initWithField:field_];
    if (![client_ canSend]) GTEST_SKIP() << "NSTextInputContext no longer takes TSM events";
  }
  // What the field answers for an index an input method asks for.
  NSRect Ask(NSUInteger index) {
    NSRect line = NSZeroRect;
    [client_ attributesForCharacterIndex:index lineHeightRectangle:&line];
    return line;
  }
  Field* field_ = nil;
  InputMethodClient* client_ = nil;
};

}  // namespace

// opencode in Terminal: the composition far into the text.
TEST_F(CompositionAnchorTest, ComposingItIsWhereTheCompositionStarts) {
  field_.length = 3000;
  field_.marked = NSMakeRange(2156, 3);
  field_.selected = NSMakeRange(2159, 0);
  const auto anchor = rdm::CompositionAnchor(client_);
  ASSERT_TRUE(anchor);
  EXPECT_EQ(field_.asked.location, 2156u);
  EXPECT_EQ(anchor->origin.x, 100 + 10.0 * 2156);
  EXPECT_EQ(anchor->size.width, 1);
  EXPECT_EQ(anchor->size.height, 15);
  // The composition's own location as the index (RDMachine Input before
  // 7.2.3) is past the composition: nothing.
  EXPECT_EQ(Ask(2156).size.height, 0);
}

// A shell prompt in Terminal: the caret near the end of the text.
TEST_F(CompositionAnchorTest, NotComposingItIsTheCaret) {
  field_.length = 52;
  field_.marked = kNotAsked;
  field_.selected = NSMakeRange(47, 0);
  const auto anchor = rdm::CompositionAnchor(client_);
  ASSERT_TRUE(anchor);
  EXPECT_EQ(field_.asked.location, 47u);
  EXPECT_EQ(anchor->origin.x, 100 + 10.0 * 47);
  // The caret's own location as the index: AppKit asks for character 94,
  // past the text.
  EXPECT_EQ(Ask(47).size.height, 0);
  EXPECT_EQ(field_.asked.location, 94u);
}

TEST_F(CompositionAnchorTest, NothingWhenTheFieldGivesNothing) {
  field_.length = 10;
  field_.marked = kNotAsked;
  field_.selected = NSMakeRange(11, 0);  // past its own text
  EXPECT_FALSE(rdm::CompositionAnchor(client_));
}
