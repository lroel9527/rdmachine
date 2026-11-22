#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

#include "input_source_icon.h"

#include <algorithm>
#include <cmath>

#include "rdmachine/ime_wire.h"

namespace rdmclient {

std::string CurrentInputSourceId() {
  TISInputSourceRef src = TISCopyCurrentKeyboardInputSource();
  if (!src) return {};
  std::string result;
  if (CFStringRef s = static_cast<CFStringRef>(TISGetInputSourceProperty(src, kTISPropertyInputSourceID))) {
    result = [(__bridge NSString*)s UTF8String] ?: "";
  }
  CFRelease(src);
  return result;
}

std::vector<uint8_t> InputSourceIconPng(const std::string& source_id) {
  NSDictionary* props = @{(__bridge NSString*)kTISPropertyInputSourceID : @(source_id.c_str())};
  CFArrayRef list = TISCreateInputSourceList((__bridge CFDictionaryRef)props, true);
  if (!list) return {};
  if (CFArrayGetCount(list) == 0) {
    CFRelease(list);
    return {};
  }
  TISInputSourceRef src = (TISInputSourceRef)CFArrayGetValueAtIndex(list, 0);
  // What macOS shows in its menu bar: an input method's icon file (whose
  // extension may be wrong: Korean names a .png that is a .tiff), else the
  // label it draws in a rounded square for a keyboard layout ("A" for ABC,
  // "US", "DE"; not a public property: absent, the next way is taken), else
  // the layout's own icon.
  NSImage* icon = nil;
  NSString* label = nil;
  if (CFURLRef url = static_cast<CFURLRef>(TISGetInputSourceProperty(src, kTISPropertyIconImageURL))) {
    NSURL* u = (__bridge NSURL*)url;
    icon = [[NSImage alloc] initWithContentsOfURL:u];
    for (NSString* ext in @[@"tiff", @"png", @"pdf", @"icns"]) {
      if (icon) break;
      icon = [[NSImage alloc] initWithContentsOfURL:[[u URLByDeletingPathExtension] URLByAppendingPathExtension:ext]];
    }
  }
  if (!icon) {
    if (CFTypeRef labels = TISGetInputSourceProperty(src, CFSTR("TISPropertyInputSourceIconLabels"))) {
      if (CFGetTypeID(labels) == CFDictionaryGetTypeID()) {
        id primary = ((__bridge NSDictionary*)labels)[@"Primary"];
        if ([primary isKindOfClass:NSString.class] && [primary length] > 0) label = primary;
      }
    }
  }
  if (!icon && !label) {
    if (IconRef ref = static_cast<IconRef>(TISGetInputSourceProperty(src, kTISPropertyIconRef))) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      icon = [[NSImage alloc] initWithIconRef:ref];  // the only way to a keyboard layout's icon
#pragma clang diagnostic pop
    }
  }
  CFRelease(list);
  if (!icon && !label) return {};

  constexpr NSInteger kSide = 32;
  NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                  pixelsWide:kSide
                                                                  pixelsHigh:kSide
                                                               bitsPerSample:8
                                                             samplesPerPixel:4
                                                                    hasAlpha:YES
                                                                    isPlanar:NO
                                                              colorSpaceName:NSDeviceRGBColorSpace
                                                                 bytesPerRow:kSide * 4
                                                                bitsPerPixel:32];
  if (!rep) return {};
  [NSGraphicsContext saveGraphicsState];
  NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
  if (icon) {
    [icon drawInRect:NSMakeRect(0, 0, kSide, kSide) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];
  } else {
    // The label cut out of a rounded square, as macOS draws it.
    [NSColor.blackColor setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(1, 3, 30, 26) xRadius:5 yRadius:5] fill];
    static constexpr CGFloat kPoints[] = {22, 22, 15, 11, 9};  // by its length
    NSDictionary* attrs = @{
      NSFontAttributeName : [NSFont systemFontOfSize:kPoints[std::min<NSUInteger>(label.length, 4)]
                                              weight:NSFontWeightSemibold],
      NSForegroundColorAttributeName : NSColor.blackColor
    };
    const NSSize ts = [label sizeWithAttributes:attrs];
    NSGraphicsContext.currentContext.compositingOperation = NSCompositingOperationDestinationOut;
    [label drawAtPoint:NSMakePoint((kSide - ts.width) / 2, (kSide - ts.height) / 2) withAttributes:attrs];
  }
  [NSGraphicsContext restoreGraphicsState];
  // A template, as the menu bar draws it: dark shapes stay, light ones are
  // let through (a keyboard layout's icon may be a light letter on dark).
  // An icon whose shapes are all light is a template drawn in white: its
  // own alpha then.
  unsigned char* px = rep.bitmapData;
  std::vector<unsigned char> alpha(kSide * kSide);
  double before = 0, after = 0;
  for (NSInteger i = 0; i < kSide * kSide; ++i) {
    unsigned char* p = px + i * 4;
    alpha[i] = p[3];
    if (p[3] == 0) continue;
    const double a = p[3] / 255.0;
    const double light = (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0 / a;  // premultiplied
    before += a;
    after += a * (1.0 - std::min(1.0, light));
    p[0] = p[1] = p[2] = 0;
    p[3] = static_cast<unsigned char>(std::lround(a * (1.0 - std::min(1.0, light)) * 255.0));
  }
  if (before > 0 && after < 0.15 * before) {
    for (NSInteger i = 0; i < kSide * kSide; ++i) px[i * 4 + 3] = alpha[i];
  }
  NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  if (!png || png.length > rdm::kImeIconMaxBytes) return {};
  const auto* bytes = static_cast<const uint8_t*>(png.bytes);
  return std::vector<uint8_t>(bytes, bytes + png.length);
}

}  // namespace rdmclient
