// The macOS clipboard: an NSPasteboard (the general one by default).
//
// NSPasteboard is used from ClipboardSync's own thread, one call at a time;
// AppKit allows that for the pasteboard, and command-line tools such as
// pbcopy use it without a run loop.
#import <AppKit/AppKit.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <cstring>

#include "rdmachine/session/system_clipboard.h"

namespace rdm {
namespace {

// http://nspasteboard.org: markers that password managers and similar apps
// put next to secrets and short-lived data, asking clipboard tools to leave
// them alone.
NSString* const kConcealedType = @"org.nspasteboard.ConcealedType";
NSString* const kTransientType = @"org.nspasteboard.TransientType";

std::string Bytes(NSData* d) { return d ? std::string(static_cast<const char*>(d.bytes), d.length) : std::string(); }

std::string Utf8(NSString* s) {
  const char* utf8 = s.UTF8String;
  return utf8 ? std::string(utf8, strlen(utf8)) : std::string();
}

NSString* NS(const std::string& s) {
  return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

NSPasteboardType TypeOf(const std::string& mime) {
  if (mime == kMimeText) return NSPasteboardTypeString;
  if (mime == kMimeRtf) return NSPasteboardTypeRTF;
  if (mime == kMimeHtml) return NSPasteboardTypeHTML;
  if (mime == kMimePng) return NSPasteboardTypePNG;
  return nil;
}

}  // namespace
}  // namespace rdm

// Answers the system when an application pastes the peer's copy: fetches
// what is asked for from the peer, keeping the run loop going meanwhile so
// the application (the client's remote window, the server's menu) stays
// responsive.
@interface RDMPromiseProvider : NSObject <NSPasteboardItemDataProvider>
- (instancetype)initWithPromise:(std::shared_ptr<rdm::ClipboardPromise>)promise;
@end

@implementation RDMPromiseProvider {
  std::shared_ptr<rdm::ClipboardPromise> _promise;
}

- (instancetype)initWithPromise:(std::shared_ptr<rdm::ClipboardPromise>)promise {
  if (!(self = [super init])) return nil;
  _promise = std::move(promise);
  return self;
}

- (void)pasteboard:(NSPasteboard*)pb item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type {
  // The system asks on the main thread; anywhere else (an in-process read
  // from another thread) there is no run loop of ours to keep going.
  std::function<void()> pump;
  if (NSThread.isMainThread) pump = [] { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false); };
  if ([type isEqualToString:NSPasteboardTypeTIFF]) {
    // For applications that only read TIFF.
    auto png = _promise->FetchFormat(std::string(rdm::kMimePng), pump);
    if (!png) return;
    NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:[NSData dataWithBytes:png->data() length:png->size()]];
    if (NSData* tiff = rep.TIFFRepresentation) [item setData:tiff forType:type];
    return;
  }
  for (const auto& mime : _promise->formats()) {
    if (![rdm::TypeOf(mime) isEqualToString:type]) continue;
    auto data = _promise->FetchFormat(mime, pump);
    if (!data) return;
    [item setData:[NSData dataWithBytes:data->data() length:data->size()] forType:type];
    return;
  }
}

@end

namespace rdm {
namespace {

class MacClipboard : public Clipboard {
 public:
  explicit MacClipboard(const std::string& name) : name_(name) {}

  int64_t ChangeCount() override {
    @autoreleasepool {
      return static_cast<int64_t>(Board().changeCount);
    }
  }

  std::optional<ClipboardContent> Read() override {
    @autoreleasepool {
      NSPasteboard* pb = Board();
      // Our own promise of the peer's copy: reading it would fetch it.
      if (pb.changeCount == promise_count_.load()) return std::nullopt;
      NSArray<NSPasteboardType>* types = pb.types;
      if ([types containsObject:kConcealedType] || [types containsObject:kTransientType]) return std::nullopt;
      ClipboardContent c;
      if (NSString* s = [pb stringForType:NSPasteboardTypeString]) {
        c.formats.push_back({std::string(kMimeText), Utf8(s)});
      }
      // Files copied in Finder: only their names (the text above). The
      // files themselves are not shared, nor Finder's icons of them.
      if ([pb canReadObjectForClasses:@[ NSURL.class ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}]) {
        return c;
      }
      if (NSData* rtf = [pb dataForType:NSPasteboardTypeRTF]) c.formats.push_back({std::string(kMimeRtf), Bytes(rtf)});
      if (NSString* html = [pb stringForType:NSPasteboardTypeHTML]) {
        c.formats.push_back({std::string(kMimeHtml), Utf8(html)});
      }
      NSData* png = [pb dataForType:NSPasteboardTypePNG];
      if (!png) {
        // Screenshots and most apps give TIFF, often uncompressed and huge.
        if (NSData* tiff = [pb dataForType:NSPasteboardTypeTIFF]) {
          NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:tiff];
          png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        }
      }
      if (png) c.formats.push_back({std::string(kMimePng), Bytes(png)});
      return c;
    }
  }

  std::optional<int64_t> Write(const ClipboardContent& content) override {
    @autoreleasepool {
      NSPasteboard* pb = Board();
      // clearContents takes ownership and returns the new change count;
      // the writes after it do not change it again.
      const NSInteger count = [pb clearContents];
      for (const auto& f : content.formats) {
        BOOL ok = YES;
        if (f.mime == kMimeText) {
          NSString* s = NS(f.data);
          ok = s && [pb setString:s forType:NSPasteboardTypeString];
        } else if (f.mime == kMimeHtml) {
          NSString* s = NS(f.data);
          ok = s && [pb setString:s forType:NSPasteboardTypeHTML];
        } else if (f.mime == kMimeRtf) {
          ok = [pb setData:[NSData dataWithBytes:f.data.data() length:f.data.size()] forType:NSPasteboardTypeRTF];
        } else if (f.mime == kMimePng) {
          ok = [pb setData:[NSData dataWithBytes:f.data.data() length:f.data.size()] forType:NSPasteboardTypePNG];
        }
        if (!ok) return std::nullopt;
      }
      return static_cast<int64_t>(count);
    }
  }

  std::optional<int64_t> WritePromise(std::shared_ptr<ClipboardPromise> promise) override {
    @autoreleasepool {
      NSPasteboard* pb = Board();
      NSMutableArray<NSPasteboardType>* formats = [NSMutableArray array];
      for (const auto& mime : promise->formats()) {
        if (NSPasteboardType t = TypeOf(mime)) [formats addObject:t];
        if (mime == kMimePng) [formats addObject:NSPasteboardTypeTIFF];
      }
      if (formats.count == 0) return std::nullopt;
      RDMPromiseProvider* provider = [[RDMPromiseProvider alloc] initWithPromise:promise];
      NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
      if (![item setDataProvider:provider forTypes:formats]) return std::nullopt;
      NSArray<NSPasteboardItem*>* items = @[ item ];
      const NSInteger count = [pb clearContents];
      if (![pb writeObjects:items]) return std::nullopt;
      {
        // One clipboard may serve several sessions, each on its own thread.
        std::lock_guard lock(provider_mu_);
        provider_ = provider;  // kept until the next promise
      }
      promise_count_ = static_cast<int64_t>(count);
      return static_cast<int64_t>(count);
    }
  }

 private:
  NSPasteboard* Board() {
    if (name_.empty()) return [NSPasteboard generalPasteboard];
    return [NSPasteboard pasteboardWithName:NS(name_)];
  }

  const std::string name_;
  std::atomic<int64_t> promise_count_{-1};  // the change count of our promise on the pasteboard
  std::mutex provider_mu_;
  RDMPromiseProvider* provider_ = nil;
};

}  // namespace

std::shared_ptr<Clipboard> SystemClipboard(const std::string& pasteboard_name) {
  return std::make_shared<MacClipboard>(pasteboard_name);
}

}  // namespace rdm
