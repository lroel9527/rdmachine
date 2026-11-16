#include "rdmachine/session/virtual_desktop.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include <chrono>
#include <thread>
#include <vector>

// CGVirtualDisplay is in CoreGraphics since macOS 10.14 but has no public
// header (other remote desktop and display tools use it the same way). The
// classes are looked up by name, so a system without them just means "no
// virtual desktop" and the server shares its own display.
@class CGVirtualDisplay;

@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(NSUInteger)width height:(NSUInteger)height refreshRate:(CGFloat)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property(retain, nonatomic) NSArray<CGVirtualDisplayMode*>* modes;  // sizes in points
@property(nonatomic) unsigned int hiDPI;  // 1: each mode also at 2 pixels per point
@end

@interface CGVirtualDisplayDescriptor : NSObject
@property(retain, nonatomic) dispatch_queue_t queue;
@property(retain, nonatomic) NSString* name;
@property(nonatomic) unsigned int maxPixelsWide;
@property(nonatomic) unsigned int maxPixelsHigh;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) unsigned int vendorID;
@property(nonatomic) unsigned int productID;
@property(nonatomic) unsigned int serialNum;
@property(copy, nonatomic) void (^terminationHandler)(id, CGVirtualDisplay*);
@end

@interface CGVirtualDisplay : NSObject
@property(readonly, nonatomic) CGDirectDisplayID displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor*)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings*)settings;
@end

namespace rdm {
namespace {

using namespace std::chrono_literals;

constexpr auto kSettleLimit = 4s;
constexpr auto kModeListLimit = 1500ms;
constexpr auto kTearDownLimit = 2s;

template <class F>
bool WaitFor(std::chrono::milliseconds limit, F done) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(20ms);
  }
  return true;
}

// The display's mode of `d`'s points and pixels (caller releases), or null.
CGDisplayModeRef CopyMode(CGDirectDisplayID id, const DesktopSpec& d) {
  NSDictionary* options = @{(__bridge NSString*)kCGDisplayShowDuplicateLowResolutionModes : @YES};
  CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, (__bridge CFDictionaryRef)options);
  if (!modes) return nullptr;
  CGDisplayModeRef found = nullptr;
  for (CFIndex i = 0; i < CFArrayGetCount(modes); ++i) {
    auto mode = static_cast<CGDisplayModeRef>(const_cast<void*>(CFArrayGetValueAtIndex(modes, i)));
    if (static_cast<int32_t>(CGDisplayModeGetWidth(mode)) == DesktopPointsWide(d) &&
        static_cast<int32_t>(CGDisplayModeGetHeight(mode)) == DesktopPointsHigh(d) &&
        static_cast<int32_t>(CGDisplayModeGetPixelWidth(mode)) == d.width &&
        static_cast<int32_t>(CGDisplayModeGetPixelHeight(mode)) == d.height) {
      found = CGDisplayModeRetain(mode);
      break;
    }
  }
  CFRelease(modes);
  return found;
}

// Whether the display now has `d`'s size. The points come from the display's
// bounds, which are always known; the pixels from its mode, when the mode
// can be read (see Apply()).
bool SizeIs(CGDirectDisplayID id, const DesktopSpec& d) {
  const CGRect b = CGDisplayBounds(id);
  if (static_cast<int32_t>(b.size.width) != DesktopPointsWide(d) ||
      static_cast<int32_t>(b.size.height) != DesktopPointsHigh(d)) {
    return false;
  }
  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
  if (!mode) return true;
  const bool same = static_cast<int32_t>(CGDisplayModeGetPixelWidth(mode)) == d.width &&
                    static_cast<int32_t>(CGDisplayModeGetPixelHeight(mode)) == d.height;
  CGDisplayModeRelease(mode);
  return same;
}

std::vector<CGDirectDisplayID> OnlineDisplays() {
  uint32_t n = 0;
  if (CGGetOnlineDisplayList(0, nullptr, &n) != kCGErrorSuccess || n == 0) return {};
  std::vector<CGDirectDisplayID> ids(n);
  if (CGGetOnlineDisplayList(n, ids.data(), &n) != kCGErrorSuccess) return {};
  ids.resize(n);
  return ids;
}

// "online, 12 modes, now 1440x900 points 2880x1800 pixels", for error messages.
std::string Describe(CGDirectDisplayID id) {
  std::string out = CGDisplayIsOnline(id) ? "online" : "not online";
  NSDictionary* options = @{(__bridge NSString*)kCGDisplayShowDuplicateLowResolutionModes : @YES};
  if (CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, (__bridge CFDictionaryRef)options)) {
    out += ", " + std::to_string(CFArrayGetCount(modes)) + " modes";
    CFRelease(modes);
  }
  if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
    out += ", now " + std::to_string(CGDisplayModeGetWidth(mode)) + "x" + std::to_string(CGDisplayModeGetHeight(mode)) +
           " points " + std::to_string(CGDisplayModeGetPixelWidth(mode)) + "x" +
           std::to_string(CGDisplayModeGetPixelHeight(mode)) + " pixels";
    CGDisplayModeRelease(mode);
  }
  return out;
}

std::string CgError(const char* what, CGError e) { return std::string(what) + " failed (CGError " + std::to_string(e) + ")"; }

class MacVirtualDesktop final : public VirtualDesktop {
 public:
  ~MacVirtualDesktop() override {
    TearDown();
    for (Saved& s : saved_) {
      if (s.mode) CGDisplayModeRelease(s.mode);
    }
  }

  // An extra display of an extended desktop instead of the main one.
  void MakeExtra(int32_t x, int32_t y, int number) {
    extra_ = true;
    x_ = x;
    y_ = y;
    number_ = number;
  }

  bool Origin(int32_t* x, int32_t* y) const override {
    if (!display_) return false;
    const CGRect b = CGDisplayBounds(display_.displayID);
    *x = static_cast<int32_t>(b.origin.x);
    *y = static_cast<int32_t>(b.origin.y);
    return true;
  }

  bool Build(const DesktopSpec& d, std::string* error) {
    @autoreleasepool {
      Class descriptor_class = NSClassFromString(@"CGVirtualDisplayDescriptor");
      Class display_class = NSClassFromString(@"CGVirtualDisplay");
      if (!descriptor_class || !display_class || !NSClassFromString(@"CGVirtualDisplaySettings") ||
          !NSClassFromString(@"CGVirtualDisplayMode")) {
        *error = "this macOS has no CGVirtualDisplay";
        return false;
      }
      // How the real displays are now, to put them back afterwards (the
      // main desktop's job; an extra display leaves them to it).
      for (CGDirectDisplayID id : extra_ ? std::vector<CGDirectDisplayID>{} : OnlineDisplays()) {
        Saved s;
        s.id = id;
        s.mirror_of = CGDisplayMirrorsDisplay(id);
        s.origin = CGDisplayBounds(id).origin;
        s.mode = CGDisplayCopyDisplayMode(id);
        saved_.push_back(s);
      }

      // Room for any later Resize().
      if (Create(d, kMaxDesktopWidth, kMaxDesktopHeight, error) && Apply(d, error)) return true;
      TearDown();
      if (d.width >= kMaxDesktopWidth && d.height >= kMaxDesktopHeight) return false;
      // Some Macs (older macOS, Intel graphics) do not take a display that
      // may grow to 8K: once more with room for just this size. A later
      // Resize() to a larger one then fails and the desktop stays as it is.
      const std::string first = *error;
      if (Create(d, d.width, d.height, error) && Apply(d, error)) return true;
      TearDown();
      *error = first + "; at most " + std::to_string(d.width) + "x" + std::to_string(d.height) + " pixels: " + *error;
      return false;
    }
  }

  DesktopSpec spec() const override { return spec_; }

  bool Resize(const DesktopSpec& d, std::string* error) override {
    @autoreleasepool {
      if (d == spec_) return true;
      const DesktopSpec before = spec_;
      if (Apply(d, error)) return true;
      std::string ignored;
      Apply(before, &ignored);
      return false;
    }
  }

 private:
  struct Saved {
    CGDirectDisplayID id = kCGNullDirectDisplay;
    CGDirectDisplayID mirror_of = kCGNullDirectDisplay;
    CGPoint origin = CGPointZero;
    CGDisplayModeRef mode = nullptr;  // owned
  };

  // The virtual display itself, room for max_wide x max_high pixels.
  bool Create(const DesktopSpec& d, int32_t max_wide, int32_t max_high, std::string* error) {
    CGVirtualDisplayDescriptor* desc =
        [(CGVirtualDisplayDescriptor*)[NSClassFromString(@"CGVirtualDisplayDescriptor") alloc] init];
    desc.queue = dispatch_queue_create("rdmachine.virtual-desktop", DISPATCH_QUEUE_SERIAL);
    desc.name = extra_ ? [NSString stringWithFormat:@"RDMachine Desktop %d", number_] : @"RDMachine Desktop";
    desc.maxPixelsWide = static_cast<unsigned int>(max_wide);
    desc.maxPixelsHigh = static_cast<unsigned int>(max_high);
    // Nominal: about 110 points per inch, an ordinary desktop screen.
    desc.sizeInMillimeters = CGSizeMake(DesktopPointsWide(d) * 25.4 / 110, DesktopPointsHigh(d) * 25.4 / 110);
    // Always the same, so macOS knows the display again next time.
    desc.vendorID = 0x5244;   // "RD"
    desc.productID = 0x4d01;  // "M", 1
    desc.serialNum = extra_ ? static_cast<unsigned int>(number_) : 1;
    display_ = [(CGVirtualDisplay*)[NSClassFromString(@"CGVirtualDisplay") alloc] initWithDescriptor:desc];
    if (!display_ || display_.displayID == kCGNullDirectDisplay) {
      display_ = nil;
      *error = "the system refused to create a virtual display";
      return false;
    }
    return true;
  }

  // Puts the real displays back and removes the virtual one, then waits
  // (briefly) until that has happened: a server that goes on to share the
  // real display after a failed build must not start capturing it while it
  // still mirrors a display that is going away (the picture stays black).
  void TearDown() {
    if (!display_) return;
    const CGDirectDisplayID id = display_.displayID;
    @autoreleasepool {
      Restore();
      display_ = nil;  // the display goes away with the object
    }
    WaitFor(kTearDownLimit, [&] {
      for (CGDirectDisplayID o : OnlineDisplays()) {
        if (o == id) return false;
      }
      for (const Saved& s : saved_) {
        if (CGDisplayIsOnline(s.id) && CGDisplayMirrorsDisplay(s.id) != s.mirror_of) return false;
      }
      return true;
    });
  }

  // Whether the virtual display is now the desktop being shared: at its
  // size, and the main display. macOS 12 may keep reporting a real display
  // as the main one although it mirrors the virtual display, which then is
  // the only display that is not a mirror, at the origin: rdaccel's display
  // 0 all the same, so that counts too.
  bool IsTheDesktop(CGDirectDisplayID id, const DesktopSpec& d) const {
    if (!SizeIs(id, d)) return false;
    if (CGMainDisplayID() == id) return true;
    const CGRect b = CGDisplayBounds(id);
    if (b.origin.x != 0 || b.origin.y != 0 || CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay) return false;
    for (const Saved& s : saved_) {
      if (s.id != id && CGDisplayIsActive(s.id) && CGDisplayMirrorsDisplay(s.id) != id) return false;
    }
    return true;
  }

  // Gives the display the one mode `d`, selects it, makes the display the
  // main one and has every real display mirror it; waits until it is so.
  bool Apply(const DesktopSpec& d, std::string* error) {
    const CGDirectDisplayID id = display_.displayID;
    // The receivers are cast to their classes: with a plain `id` the compiler
    // may take another class's method of the same name, with other argument
    // types, and the display then gets a mode of nonsense size.
    CGVirtualDisplaySettings* settings =
        [(CGVirtualDisplaySettings*)[NSClassFromString(@"CGVirtualDisplaySettings") alloc] init];
    settings.hiDPI = d.scale >= 1.5 ? 1 : 0;
    CGVirtualDisplayMode* mode_wanted =
        [(CGVirtualDisplayMode*)[NSClassFromString(@"CGVirtualDisplayMode") alloc]
            initWithWidth:static_cast<NSUInteger>(DesktopPointsWide(d))
                   height:static_cast<NSUInteger>(DesktopPointsHigh(d))
              refreshRate:60];
    settings.modes = @[ mode_wanted ];
    if (![display_ applySettings:settings]) {
      *error = "the virtual display refused " + FormatDesktopSpec(d);
      return false;
    }
    // The mode to select: the one of d's points and pixels. A process only
    // learns a new display's modes through AppKit's event loop (the server
    // runs one on its main thread, also with --headless); in a process
    // without one the list stays empty. The display then keeps the mode it
    // chose itself, which with a single mode is the wanted one: the HiDPI
    // variant when hiDPI is set.
    CGDisplayModeRef mode = nullptr;
    WaitFor(kModeListLimit, [&] { return CGDisplayIsOnline(id) && (mode = CopyMode(id, d)) != nullptr; });
    if (!CGDisplayIsOnline(id)) {
      *error = "the virtual display did not come online (" + Describe(id) + ")";
      return false;
    }

    CGDisplayConfigRef config = nullptr;
    CGError e = CGBeginDisplayConfiguration(&config);
    if (e != kCGErrorSuccess) {
      if (mode) CGDisplayModeRelease(mode);
      *error = CgError("CGBeginDisplayConfiguration", e);
      return false;
    }
    if (mode) CGConfigureDisplayWithDisplayMode(config, id, mode, nullptr);
    if (extra_) {
      // Beside the main desktop, showing a picture of its own.
      CGConfigureDisplayMirrorOfDisplay(config, id, kCGNullDirectDisplay);
      CGConfigureDisplayOrigin(config, id, x_, y_);
    } else {
      CGConfigureDisplayOrigin(config, id, 0, 0);  // the display at the origin is the main one
      // The real displays (those there before) mirror it; not the extra
      // displays of an extended desktop, built after it.
      for (const Saved& s : saved_) {
        if (s.id != id && CGDisplayIsOnline(s.id)) CGConfigureDisplayMirrorOfDisplay(config, s.id, id);
      }
    }
    // For this login session only: nothing is written to the display
    // preferences, and it ends with the virtual display anyway.
    e = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
    if (mode) CGDisplayModeRelease(mode);
    if (e != kCGErrorSuccess) {
      *error = CgError("configuring the displays", e);
      return false;
    }
    if (extra_) {
      if (!WaitFor(kSettleLimit, [&] { return SizeIs(id, d) && CGDisplayMirrorsDisplay(id) == kCGNullDirectDisplay; })) {
        *error = "the extra display did not take " + FormatDesktopSpec(d) + " (" + Describe(id) + ")";
        return false;
      }
      spec_ = d;
      return true;
    }
    if (!WaitFor(kSettleLimit, [&] { return IsTheDesktop(id, d); })) {
      *error = "the virtual display did not become the main display at " + FormatDesktopSpec(d) + " (" + Describe(id) +
               ")";
      return false;
    }
    spec_ = d;
    return true;
  }

  // The real displays as they were: no longer mirrors of the virtual one,
  // back in their places and modes.
  void Restore() {
    if (!display_) return;
    CGDisplayConfigRef config = nullptr;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) return;
    for (const Saved& s : saved_) {
      if (!CGDisplayIsOnline(s.id)) continue;  // unplugged meanwhile
      CGConfigureDisplayMirrorOfDisplay(config, s.id, s.mirror_of);
      if (s.mirror_of == kCGNullDirectDisplay) {
        if (s.mode) CGConfigureDisplayWithDisplayMode(config, s.id, s.mode, nullptr);
        CGConfigureDisplayOrigin(config, s.id, static_cast<int32_t>(s.origin.x), static_cast<int32_t>(s.origin.y));
      }
    }
    CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
  }

  CGVirtualDisplay* display_ = nil;
  DesktopSpec spec_;
  bool extra_ = false;
  int32_t x_ = 0, y_ = 0;
  int number_ = 1;
  std::vector<Saved> saved_;
};

}  // namespace

VirtualDesktopFactory SystemVirtualDesktop() {
  return [](const DesktopSpec& planned, std::string* error) -> std::unique_ptr<VirtualDesktop> {
    auto desktop = std::make_unique<MacVirtualDesktop>();
    std::string why;
    if (!desktop->Build(planned, &why)) {
      if (error) *error = why;
      return nullptr;
    }
    return desktop;
  };
}

ExtraDesktopFactory SystemExtraDesktop() {
  return [](const DesktopSpec& planned, int32_t x, int32_t y, int number,
            std::string* error) -> std::unique_ptr<VirtualDesktop> {
    auto desktop = std::make_unique<MacVirtualDesktop>();
    desktop->MakeExtra(x, y, number);
    std::string why;
    if (!desktop->Build(planned, &why)) {
      if (error) *error = why;
      return nullptr;
    }
    return desktop;
  };
}

namespace {

class MacDisplayAwake final : public DisplayAwake {
 public:
  explicit MacDisplayAwake(const std::string& reason) {
    CFStringRef why = CFStringCreateWithCString(nullptr, reason.c_str(), kCFStringEncodingUTF8);
    IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                why ? why : CFSTR("rdmachine session"), &id_);
    // Wakes a display that is asleep (the assertion alone keeps it asleep).
    IOPMAssertionID activity = kIOPMNullAssertionID;
    if (IOPMAssertionDeclareUserActivity(why ? why : CFSTR("rdmachine session"), kIOPMUserActiveLocal, &activity) ==
            kIOReturnSuccess &&
        activity != kIOPMNullAssertionID) {
      IOPMAssertionRelease(activity);
    }
    if (why) CFRelease(why);
  }
  ~MacDisplayAwake() override {
    if (id_ != kIOPMNullAssertionID) IOPMAssertionRelease(id_);
  }

 private:
  IOPMAssertionID id_ = kIOPMNullAssertionID;
};

}  // namespace

std::unique_ptr<DisplayAwake> KeepDisplaysAwake(const std::string& reason) {
  return std::make_unique<MacDisplayAwake>(reason);
}

}  // namespace rdm
