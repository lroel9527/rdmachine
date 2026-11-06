// The macOS clipboard: the general NSPasteboard, plain text only.
//
// Not yet built or tried on a Mac (phase 1 checks the Mac parts later, see
// docs/decisions.md). NSPasteboard is used from ClipboardSync's own thread,
// one call at a time; AppKit allows that for the pasteboard, and command-line
// tools such as pbcopy use it without a run loop.
#import <AppKit/AppKit.h>

#include <cstring>

#include "rdmachine/session/system_clipboard.h"

namespace rdm {
namespace {

// http://nspasteboard.org: markers that password managers and similar apps
// put next to secrets and short-lived data, asking clipboard tools to leave
// them alone.
NSString* const kConcealedType = @"org.nspasteboard.ConcealedType";
NSString* const kTransientType = @"org.nspasteboard.TransientType";

class MacClipboard : public Clipboard {
 public:
  int64_t ChangeCount() override {
    @autoreleasepool {
      return static_cast<int64_t>([NSPasteboard generalPasteboard].changeCount);
    }
  }

  std::optional<std::string> ReadText() override {
    @autoreleasepool {
      NSPasteboard* pb = [NSPasteboard generalPasteboard];
      NSArray<NSPasteboardType>* types = pb.types;
      if ([types containsObject:kConcealedType] || [types containsObject:kTransientType]) return std::nullopt;
      NSString* s = [pb stringForType:NSPasteboardTypeString];
      if (!s) return std::nullopt;
      const char* utf8 = s.UTF8String;
      if (!utf8) return std::nullopt;
      return std::string(utf8, strlen(utf8));
    }
  }

  std::optional<int64_t> WriteText(const std::string& text) override {
    @autoreleasepool {
      NSString* s = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
      if (!s) return std::nullopt;
      NSPasteboard* pb = [NSPasteboard generalPasteboard];
      // clearContents takes ownership and returns the new change count;
      // setString does not change it again.
      const NSInteger count = [pb clearContents];
      if (![pb setString:s forType:NSPasteboardTypeString]) return std::nullopt;
      return static_cast<int64_t>(count);
    }
  }
};

}  // namespace

std::shared_ptr<Clipboard> SystemClipboard() { return std::make_shared<MacClipboard>(); }

}  // namespace rdm
