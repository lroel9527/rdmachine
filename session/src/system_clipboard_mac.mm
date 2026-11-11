// The macOS clipboard: an NSPasteboard (the general one by default).
//
// NSPasteboard is used from ClipboardSync's own thread, one call at a time;
// AppKit allows that for the pasteboard, and command-line tools such as
// pbcopy use it without a run loop.
#import <AppKit/AppKit.h>

#include <sys/xattr.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

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

// As a browser marks a download, so opening a received app or script asks
// first (Gatekeeper). Applied to everything inside a received folder too.
void MarkDownloaded(const std::string& path) {
  char value[64];
  snprintf(value, sizeof value, "0081;%08lx;RDMachine;", static_cast<unsigned long>(time(nullptr)));
  auto mark = [&](const std::filesystem::path& p) {
    setxattr(p.c_str(), "com.apple.quarantine", value, strlen(value), 0, XATTR_NOFOLLOW);
  };
  mark(path);
  std::error_code ec;
  if (!std::filesystem::is_directory(std::filesystem::symlink_status(path, ec))) return;
  for (std::filesystem::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
    mark(it->path());
  }
}

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
      NSArray<NSPasteboardType>* types = pb.types;
      if ([types containsObject:kConcealedType] || [types containsObject:kTransientType]) return std::nullopt;
      ClipboardContent c;
      NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[ NSURL.class ]
                                                options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
      for (NSURL* url in urls) {
        // Finder puts file reference URLs (file:///.file/id=...).
        NSString* path = url.filePathURL.path;
        if (path.length > 0) c.files.push_back(Utf8(path));
      }
      if (NSString* s = [pb stringForType:NSPasteboardTypeString]) {
        c.formats.push_back({std::string(kMimeText), Utf8(s)});
      }
      // With files only their names: Finder adds icons and such.
      if (!c.files.empty()) return c;
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
      if (!content.files.empty()) {
        NSMutableArray<NSURL*>* urls = [NSMutableArray array];
        for (const auto& f : content.files) {
          MarkDownloaded(f);
          NSString* path = NS(f);
          if (path) [urls addObject:[NSURL fileURLWithPath:path]];
        }
        if (![pb writeObjects:urls]) return std::nullopt;
      }
      // The formats go on the first item (Finder puts the names there too).
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

 private:
  NSPasteboard* Board() {
    if (name_.empty()) return [NSPasteboard generalPasteboard];
    return [NSPasteboard pasteboardWithName:NS(name_)];
  }

  const std::string name_;
};

}  // namespace

std::shared_ptr<Clipboard> SystemClipboard(const std::string& pasteboard_name) {
  return std::make_shared<MacClipboard>(pasteboard_name);
}

std::string ClipboardStagingDir() {
  @autoreleasepool {
    NSURL* caches = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    if (!caches) return {};
    return Utf8([caches URLByAppendingPathComponent:@"RDMachine/Clipboard" isDirectory:YES].path);
  }
}

}  // namespace rdm
