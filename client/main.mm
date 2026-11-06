// rdmachine-client: the Mac client. A connect window (server, account and
// password or pairing code), then the remote screen in a window that sends
// this Mac's mouse, scroll wheel and keyboard back.
//
// Connecting goes through rdm::RemoteSession (session/), the same code the
// command-line rdmachine-connect and the Linux tests use: TLS 1.3 control
// connection, the server's fingerprint checked against ~/Library/Application
// Support/rdmachine/known_hosts (a first-time fingerprint is shown for
// confirmation; a changed one is refused), login, then the rdaccel picture
// over UDP, falling back to TCP.
//
// The picture and input handling come from rdaccel's examples/remote_mac:
// pictures go into two IOSurfaces that Core Animation shows directly, each
// frame copying only what changed; keys are sent as physical keys (USB HID
// codes), so the remote Mac's layout and input method decide what is typed.
// Over the picture the pointer is this Mac's own, in the remote cursor's
// shape; outside the window the remote cursor is drawn where it is.
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

#include <rdaccel/rdaccel.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "rdmachine/protocol.h"
#include "rdmachine/session/config_store.h"
#include "rdmachine/session/remote_session.h"
#include "rdmachine/session/system_clipboard.h"

namespace {

NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

uint32_t Modifiers(NSEventModifierFlags f) {
  uint32_t m = 0;
  if (f & NSEventModifierFlagShift) m |= RDA_MOD_SHIFT;
  if (f & NSEventModifierFlagControl) m |= RDA_MOD_CTRL;
  if (f & NSEventModifierFlagOption) m |= RDA_MOD_ALT;
  if (f & NSEventModifierFlagCommand) m |= RDA_MOD_META;
  if (f & NSEventModifierFlagCapsLock) m |= RDA_MOD_CAPS;
  return m;
}

// The modifier flag that a modifier key's keyCode (kVK_*) changes, or 0.
NSEventModifierFlags FlagOfModifierKey(unsigned short keyCode) {
  switch (keyCode) {
    case 56: case 60: return NSEventModifierFlagShift;    // kVK_Shift, kVK_RightShift
    case 59: case 62: return NSEventModifierFlagControl;  // kVK_Control, kVK_RightControl
    case 58: case 61: return NSEventModifierFlagOption;   // kVK_Option, kVK_RightOption
    case 55: case 54: return NSEventModifierFlagCommand;  // kVK_Command, kVK_RightCommand
    case 57: return NSEventModifierFlagCapsLock;          // kVK_CapsLock
    case 63: return NSEventModifierFlagFunction;          // kVK_Function
  }
  return 0;
}

uint32_t ButtonOf(NSEvent* e) {
  switch (e.buttonNumber) {
    case 0: return RDA_BUTTON_LEFT;
    case 1: return RDA_BUTTON_RIGHT;
    case 2: return RDA_BUTTON_MIDDLE;
    case 3: return RDA_BUTTON_BACK;
    case 4: return RDA_BUTTON_FORWARD;
  }
  return 0;
}

rda_input_event Event(rda_input_kind kind) {
  rda_input_event ev;
  rda_input_event_default(&ev);
  ev.kind = kind;
  return ev;
}

IOSurfaceRef MakeSurface(int32_t w, int32_t h) {
  NSDictionary* props = @{
    (id)kIOSurfaceWidth : @(w),
    (id)kIOSurfaceHeight : @(h),
    (id)kIOSurfaceBytesPerElement : @4,
    (id)kIOSurfacePixelFormat : @(0x42475241u),  // 'BGRA'
  };
  IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
  if (s) IOSurfaceSetValue(s, kIOSurfaceColorSpace, kCGColorSpaceSRGB);
  return s;
}

// Remembers `r` as owed by a surface; many small areas become one.
void Owe(std::vector<rda_rect>& owed, const rda_rect& r, int32_t w, int32_t h) {
  if (owed.size() == 1 && owed[0].x == 0 && owed[0].y == 0 && owed[0].w == w && owed[0].h == h) return;
  if (owed.size() >= 64 || (r.x <= 0 && r.y <= 0 && r.w >= w && r.h >= h)) {
    owed.assign(1, rda_rect{0, 0, w, h});
    return;
  }
  owed.push_back(r);
}

NSString* DescribeError(const rdm::ConnectResult& r) {
  const std::string& e = r.error;
  NSString* text;
  if (e == "connect_failed") text = @"Could not reach the server.";
  else if (e == "bad_credentials") text = @"Wrong account, password or pairing code.";
  else if (e == "not_console_user") text = @"Only the user logged in on that Mac can connect for now.";
  else if (e == "code_expired") text = @"The pairing code has expired or was already used. Ask for a new one.";
  else if (e == "rate_limited")
    text = [NSString stringWithFormat:@"Too many failed attempts. Try again in %lld seconds.",
                                      static_cast<long long>((r.retry_after_ms.value_or(60000) + 999) / 1000)];
  else if (e == "method_unsupported") text = @"The server does not accept this way of logging in.";
  else if (e == "busy") text = @"Someone else is connected to that Mac.";
  else if (e == "capture_denied")
    text = @"The server may not record its screen. On that Mac, allow rdmachine-server under System Settings > "
           @"Privacy & Security > Screen Recording.";
  else if (e == "no_display") text = @"The server has no display to share.";
  else if (e == "incompatible") text = @"The server runs another version of rdmachine. Update both.";
  else if (e == "media_failed") text = @"Logged in, but the picture connection did not come up (firewall?).";
  else if (e == "untrusted_server") text = @"Connection cancelled.";
  else if (e == "timeout") text = @"The server did not answer in time.";
  else text = [NSString stringWithFormat:@"Connection failed (%@).", NS(e)];
  if (!r.detail.empty()) text = [text stringByAppendingFormat:@"\n\n%@", NS(r.detail)];
  return text;
}

}  // namespace

@class RemoteWindowController;

// The picture, the remote cursor on top, and all local input.
@interface RemoteView : NSView
@property(nonatomic, weak) RemoteWindowController* owner;
@property(nonatomic) BOOL pointerInside;
@end

// One session's window. Viewer callbacks (library threads) reach it through
// the C functions below; it is created before the session starts so pictures
// arriving during the handshake have somewhere to go.
@interface RemoteWindowController : NSObject <NSWindowDelegate>
@property(nonatomic, readonly) NSWindow* window;
@property(nonatomic, copy) void (^onClose)(void);
- (instancetype)initWithTitle:(NSString*)title;
- (void)attachSession:(std::unique_ptr<rdm::RemoteSession>)session;
- (void)endSession;
- (void)send:(const rda_input_event&)ev;
- (BOOL)streamPoint:(NSPoint)p x:(int32_t*)x y:(int32_t*)y;
- (NSCursor*)remoteCursor;
- (void)placeCursor;
- (void)key:(NSEvent*)e;
// Library threads.
- (void)frame:(const rda_image*)img changed:(const rda_rect*)changed count:(size_t)count;
- (void)streamInfo:(int32_t)w height:(int32_t)h;
- (void)cursor:(const rda_cursor_state*)c;
@end

static void OnStreamInfo(void* user, int32_t w, int32_t h) {
  [(__bridge RemoteWindowController*)user streamInfo:w height:h];
}
static void OnFrame(void* user, const rda_image* img, const rda_rect* changed, size_t n, const rda_frame_info*) {
  [(__bridge RemoteWindowController*)user frame:img changed:changed count:n];
}
static void OnCursor(void* user, const rda_cursor_state* c) { [(__bridge RemoteWindowController*)user cursor:c]; }

@implementation RemoteView {
  NSTrackingArea* _tracking;
  double _scrollX, _scrollY;  // fractions of a line not sent yet
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  if (_tracking) [self removeTrackingArea:_tracking];
  _tracking = [[NSTrackingArea alloc]
      initWithRect:self.bounds
           options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                   NSTrackingInVisibleRect
             owner:self
          userInfo:nil];
  [self addTrackingArea:_tracking];
}

- (void)resetCursorRects {
  NSCursor* c = [self.owner remoteCursor];
  [self addCursorRect:self.bounds cursor:c ?: NSCursor.arrowCursor];
}

- (void)mouseEntered:(NSEvent*)e {
  self.pointerInside = YES;
  [self.owner placeCursor];
}
- (void)mouseExited:(NSEvent*)e {
  self.pointerInside = NO;
  [self.owner placeCursor];
}

- (void)pointer:(NSEvent*)e kind:(rda_input_kind)kind {
  rda_input_event ev = Event(kind);
  const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  if (![self.owner streamPoint:p x:&ev.x y:&ev.y]) return;
  if (kind != RDA_INPUT_MOUSE_MOVE) {
    ev.code = ButtonOf(e);
    if (ev.code == 0) return;
  }
  ev.modifiers = Modifiers(e.modifierFlags);
  [self.owner send:ev];
}

- (void)mouseMoved:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_MOVE]; }
- (void)mouseDragged:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_MOVE]; }
- (void)rightMouseDragged:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_MOVE]; }
- (void)otherMouseDragged:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_MOVE]; }
- (void)mouseDown:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_DOWN]; }
- (void)mouseUp:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_UP]; }
- (void)rightMouseDown:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_DOWN]; }
- (void)rightMouseUp:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_UP]; }
- (void)otherMouseDown:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_DOWN]; }
- (void)otherMouseUp:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_UP]; }

// The wire carries whole lines (wheel notches), y > 0 up. A trackpad gives
// points; about 10 points make a line. Fractions are carried over.
- (void)scrollWheel:(NSEvent*)e {
  rda_input_event ev = Event(RDA_INPUT_SCROLL);
  const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  if (![self.owner streamPoint:p x:&ev.x y:&ev.y]) return;
  const double per_line = e.hasPreciseScrollingDeltas ? 10.0 : 1.0;
  _scrollX += -e.scrollingDeltaX / per_line;  // AppKit: x > 0 is left
  _scrollY += e.scrollingDeltaY / per_line;
  ev.delta_x = static_cast<int32_t>(std::trunc(_scrollX));
  ev.delta_y = static_cast<int32_t>(std::trunc(_scrollY));
  _scrollX -= ev.delta_x;
  _scrollY -= ev.delta_y;
  if (e.phase == NSEventPhaseEnded || e.momentumPhase == NSEventPhaseEnded) _scrollX = _scrollY = 0;
  if (ev.delta_x == 0 && ev.delta_y == 0) return;
  ev.modifiers = Modifiers(e.modifierFlags);
  [self.owner send:ev];
}

@end

@implementation RemoteWindowController {
  RemoteView* _view;
  CALayer* _layer;
  CALayer* _cursorLayer;
  CGImageRef _cursorImage;  // owned, main thread
  NSCursor* _nsCursor;
  int32_t _cursorX, _cursorY, _cursorVisible;
  int32_t _cursorW, _cursorH, _hotX, _hotY;
  int32_t _streamW, _streamH;  // main thread
  id _keyMonitor;

  std::unique_ptr<rdm::RemoteSession> _session;  // main thread
  rda_viewer* _viewer;                           // _session's, while attached
  std::set<uint32_t> _heldKeys, _heldButtons;    // main thread
  int32_t _lastX, _lastY;

  // Two surfaces: the decode thread fills the one not on screen with what
  // changed since it was last filled, the main thread shows it.
  std::mutex _surfMu;
  IOSurfaceRef _surf[2];
  int32_t _surfW, _surfH;
  std::vector<rda_rect> _owed[2];
  int _front;
  std::atomic<bool> _presentPending;
}

- (instancetype)initWithTitle:(NSString*)title {
  if (!(self = [super init])) return nil;
  _front = -1;
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 1280, 800)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = title;
  _window.releasedWhenClosed = NO;
  _window.delegate = self;
  _window.acceptsMouseMovedEvents = YES;
  _view = [[RemoteView alloc] initWithFrame:_window.contentView.bounds];
  _view.owner = self;
  _view.wantsLayer = YES;
  _window.contentView = _view;
  _layer = _view.layer;
  _layer.backgroundColor = NSColor.blackColor.CGColor;
  _layer.contentsGravity = kCAGravityResizeAspect;
  _cursorLayer = [CALayer layer];
  _cursorLayer.anchorPoint = CGPointZero;
  _cursorLayer.hidden = YES;
  [_layer addSublayer:_cursorLayer];
  return self;
}

- (void)attachSession:(std::unique_ptr<rdm::RemoteSession>)session {
  _session = std::move(session);
  _viewer = _session->viewer();
  [_window makeKeyAndOrderFront:nil];
  [_window makeFirstResponder:_view];
  // Keys go through a local monitor rather than keyDown: it also sees
  // Command shortcuts (and their key-up, which AppKit otherwise swallows).
  __weak RemoteWindowController* weakSelf = self;
  _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp |
                                                              NSEventMaskFlagsChanged
                                                      handler:^NSEvent*(NSEvent* e) {
                                                        RemoteWindowController* me = weakSelf;
                                                        if (!me || e.window != me.window) return e;
                                                        [me key:e];
                                                        return nil;
                                                      }];
}

// Main thread. Lets go of held keys, says bye, frees everything. No viewer
// callback runs after it returns.
- (void)endSession {
  if (_keyMonitor) {
    [NSEvent removeMonitor:_keyMonitor];
    _keyMonitor = nil;
  }
  [self releaseHeld];
  _viewer = nullptr;
  if (_session) {
    _session->Close();
    _session.reset();
  }
  if (_cursorImage) CGImageRelease(_cursorImage);
  _cursorImage = nullptr;
  std::lock_guard<std::mutex> lock(_surfMu);
  for (auto& s : _surf) {
    if (s) CFRelease(s);
    s = nullptr;
  }
}

- (void)dealloc {
  [self endSession];
}

- (void)windowWillClose:(NSNotification*)note {
  [self endSession];
  if (self.onClose) self.onClose();
}

- (void)windowDidResignKey:(NSNotification*)note {
  [self releaseHeld];
}

- (void)windowDidResize:(NSNotification*)note {
  [self rebuildCursor];
  [self placeCursor];
}

// Library thread -> main thread. Sizes the window to the stream once, at
// most 85% of the screen.
- (void)streamInfo:(int32_t)w height:(int32_t)h {
  __weak RemoteWindowController* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    [weakSelf streamSizeChanged:w height:h];
  });
}

- (void)streamSizeChanged:(int32_t)w height:(int32_t)h {
  if (w == _streamW && h == _streamH) return;
  const bool first = _streamW == 0;
  _streamW = w;
  _streamH = h;
  [self rebuildCursor];
  if (!first || w <= 0 || h <= 0) return;
  const NSRect screen = (_window.screen ?: NSScreen.mainScreen).visibleFrame;
  const double scale = _window.backingScaleFactor > 0 ? _window.backingScaleFactor : 1;
  double cw = w / scale, ch = h / scale;  // one stream pixel per backing pixel
  const double fit = std::min({1.0, screen.size.width * 0.85 / cw, screen.size.height * 0.85 / ch});
  [_window setContentSize:NSMakeSize(cw * fit, ch * fit)];
  _window.contentAspectRatio = NSMakeSize(w, h);
  [_window center];
}

- (BOOL)fitScale:(double*)s ox:(double*)ox oy:(double*)oy {
  const NSSize b = _view.bounds.size;
  if (_streamW <= 0 || _streamH <= 0 || b.width <= 0 || b.height <= 0) return NO;
  *s = std::min(b.width / _streamW, b.height / _streamH);
  *ox = (b.width - _streamW * *s) / 2;
  *oy = (b.height - _streamH * *s) / 2;
  return YES;
}

- (BOOL)streamPoint:(NSPoint)p x:(int32_t*)x y:(int32_t*)y {
  double s, ox, oy;
  if (![self fitScale:&s ox:&ox oy:&oy]) return NO;
  const double sx = (p.x - ox) / s, sy = _streamH - (p.y - oy) / s;  // view origin is bottom left
  if (sx < 0 || sy < 0 || sx >= _streamW || sy >= _streamH) return NO;
  *x = static_cast<int32_t>(sx);
  *y = static_cast<int32_t>(sy);
  return YES;
}

- (void)send:(const rda_input_event&)ev {
  if (!_viewer) return;
  switch (ev.kind) {
    case RDA_INPUT_MOUSE_DOWN: _heldButtons.insert(ev.code); break;
    case RDA_INPUT_MOUSE_UP: _heldButtons.erase(ev.code); break;
    case RDA_INPUT_KEY_DOWN: _heldKeys.insert(ev.code); break;
    case RDA_INPUT_KEY_UP: _heldKeys.erase(ev.code); break;
    default: break;
  }
  if (ev.kind != RDA_INPUT_KEY_DOWN && ev.kind != RDA_INPUT_KEY_UP) {
    _lastX = ev.x;
    _lastY = ev.y;
  }
  rda_viewer_send_input(_viewer, &ev);
}

- (void)key:(NSEvent*)e {
  rda_input_event ev = Event(RDA_INPUT_KEY_DOWN);
  ev.modifiers = Modifiers(e.modifierFlags);
  ev.code = rda_keycode_from_mac(e.keyCode);
  if (ev.code == 0) return;  // no HID equivalent (e.g. Fn)
  if (e.type == NSEventTypeFlagsChanged) {
    const NSEventModifierFlags flag = FlagOfModifierKey(e.keyCode);
    if (flag == 0) return;
    if (flag == NSEventModifierFlagCapsLock) {
      // One flagsChanged per press: the host toggles on a full press.
      [self send:ev];
      ev.kind = RDA_INPUT_KEY_UP;
      [self send:ev];
      return;
    }
    if (_heldKeys.count(ev.code)) {
      ev.kind = RDA_INPUT_KEY_UP;
    } else if (!(e.modifierFlags & flag)) {
      return;
    }
  } else {
    ev.kind = e.type == NSEventTypeKeyDown ? RDA_INPUT_KEY_DOWN : RDA_INPUT_KEY_UP;
  }
  [self send:ev];  // auto-repeat arrives as further key-downs
}

- (void)releaseHeld {
  const std::set<uint32_t> keys = _heldKeys, buttons = _heldButtons;
  for (uint32_t k : keys) {
    rda_input_event ev = Event(RDA_INPUT_KEY_UP);
    ev.code = k;
    [self send:ev];
  }
  for (uint32_t b : buttons) {
    rda_input_event ev = Event(RDA_INPUT_MOUSE_UP);
    ev.code = b;
    ev.x = _lastX;
    ev.y = _lastY;
    [self send:ev];
  }
}

// Decode thread. Copies what changed into the surface that is not on screen
// and hands it to the main thread; while the main thread is still busy with
// the previous one, only remembers what changed.
- (void)frame:(const rda_image*)img changed:(const rda_rect*)changed count:(size_t)count {
  std::unique_lock<std::mutex> lock(_surfMu);
  if (img->width != _surfW || img->height != _surfH || !_surf[0]) {
    for (auto& s : _surf) {
      if (s) CFRelease(s);
      s = MakeSurface(img->width, img->height);
    }
    _surfW = img->width;
    _surfH = img->height;
    _owed[0].assign(1, rda_rect{0, 0, img->width, img->height});
    _owed[1] = _owed[0];
    _front = -1;
    if (!_surf[0] || !_surf[1]) return;
  } else {
    for (auto& o : _owed) {
      if (count == 0) Owe(o, rda_rect{0, 0, img->width, img->height}, img->width, img->height);
      for (size_t i = 0; i < count; ++i) Owe(o, changed[i], img->width, img->height);
    }
  }
  if (_presentPending.exchange(true)) return;
  const int back = _front == 0 ? 1 : 0;
  IOSurfaceRef s = _surf[back];
  IOSurfaceLock(s, 0, nullptr);
  auto* dst = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(s));
  const size_t dst_stride = IOSurfaceGetBytesPerRow(s);
  for (const auto& r0 : _owed[back]) {
    const int32_t x0 = std::clamp(r0.x, 0, img->width), y0 = std::clamp(r0.y, 0, img->height);
    const int32_t x1 = std::clamp(r0.x + r0.w, 0, img->width), y1 = std::clamp(r0.y + r0.h, 0, img->height);
    if (x1 <= x0) continue;
    for (int32_t y = y0; y < y1; ++y) {
      std::memcpy(dst + static_cast<size_t>(y) * dst_stride + static_cast<size_t>(x0) * 4,
                  img->data + static_cast<size_t>(y) * img->stride + static_cast<size_t>(x0) * 4,
                  static_cast<size_t>(x1 - x0) * 4);
    }
  }
  _owed[back].clear();
  IOSurfaceUnlock(s, 0, nullptr);
  CFRetain(s);
  lock.unlock();
  __weak RemoteWindowController* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    RemoteWindowController* me = weakSelf;
    if (me) [me show:s back:back];
    CFRelease(s);
  });
}

- (void)show:(IOSurfaceRef)s back:(int)back {
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _layer.contents = (__bridge id)s;
  [CATransaction commit];
  {
    std::lock_guard<std::mutex> l(_surfMu);
    _front = back;
  }
  _presentPending = false;
}

+ (CGImageRef)makeCursorImage:(const rda_cursor_image*)c {
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, c->bgra, static_cast<CFIndex>(c->width) * c->height * 4);
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef image = CGImageCreate(c->width, c->height, 8, 32, static_cast<size_t>(c->width) * 4, cs,
                                   static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
                                       static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst),
                                   provider, nullptr, false, kCGRenderingIntentDefault);
  CGColorSpaceRelease(cs);
  CGDataProviderRelease(provider);
  CFRelease(data);
  return image;
}

// Transport thread: the image pointer is only valid during the call.
- (void)cursor:(const rda_cursor_state*)c {
  CGImageRef image = nullptr;
  if (c->image_changed && c->image) image = [RemoteWindowController makeCursorImage:c->image];
  const int32_t x = c->x, y = c->y, visible = c->visible;
  const int32_t iw = c->image ? c->image->width : -1, ih = c->image ? c->image->height : -1;
  const int32_t hx = c->image ? c->image->hotspot_x : 0, hy = c->image ? c->image->hotspot_y : 0;
  __weak RemoteWindowController* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    RemoteWindowController* me = weakSelf;
    if (!me) {
      if (image) CGImageRelease(image);
      return;
    }
    [me cursorAt:x y:y visible:visible image:image width:iw height:ih hotX:hx hotY:hy];
  });
}

- (NSCursor*)remoteCursor {
  return _nsCursor;
}

- (void)rebuildCursor {
  double s, ox, oy;
  if (!_cursorImage || _cursorW <= 0 || ![self fitScale:&s ox:&ox oy:&oy]) return;
  NSImage* image = [[NSImage alloc] initWithCGImage:_cursorImage
                                               size:NSMakeSize(std::max(1.0, _cursorW * s),
                                                               std::max(1.0, _cursorH * s))];
  _nsCursor = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(_hotX * s, _hotY * s)];
  [_window invalidateCursorRectsForView:_view];
}

- (void)cursorAt:(int32_t)x
               y:(int32_t)y
         visible:(int32_t)visible
           image:(CGImageRef)image
           width:(int32_t)iw
          height:(int32_t)ih
            hotX:(int32_t)hx
            hotY:(int32_t)hy {
  _cursorX = x;
  _cursorY = y;
  _cursorVisible = visible;
  if (iw >= 0) {
    _cursorW = iw;
    _cursorH = ih;
    _hotX = hx;
    _hotY = hy;
  }
  if (image) {
    if (_cursorImage) CGImageRelease(_cursorImage);
    _cursorImage = image;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _cursorLayer.contents = (__bridge id)image;
    [CATransaction commit];
    [self rebuildCursor];
  }
  [self placeCursor];
}

- (void)placeCursor {
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  double s, ox, oy;
  if (!_view.pointerInside && _cursorW > 0 && [self fitScale:&s ox:&ox oy:&oy]) {
    _cursorLayer.frame = CGRectMake(ox + (_cursorX - _hotX) * s, oy + (_streamH - (_cursorY - _hotY) - _cursorH) * s,
                                    _cursorW * s, _cursorH * s);
    _cursorLayer.hidden = !_cursorVisible;
  } else {
    _cursorLayer.hidden = YES;
  }
  [CATransaction commit];
}

@end

// ---------------------------------------------------------------------------
// Connect window and application

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate {
  NSWindow* _connectWindow;
  NSTextField* _host;
  NSTextField* _user;
  NSSecureTextField* _password;
  NSButton* _usePairing;
  NSTextField* _code;
  NSButton* _connectButton;
  NSProgressIndicator* _spinner;
  RemoteWindowController* _remote;
  std::unique_ptr<rdm::KnownHosts> _knownHosts;
}

static NSTextField* Label(NSString* text, NSRect frame) {
  NSTextField* l = [NSTextField labelWithString:text];
  l.frame = frame;
  l.alignment = NSTextAlignmentRight;
  return l;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  _knownHosts = std::make_unique<rdm::KnownHosts>(rdm::DefaultConfigDir() + "/known_hosts");
  [self buildMenu];
  _connectWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 420, 230)
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
  _connectWindow.title = @"rdmachine";
  _connectWindow.releasedWhenClosed = NO;
  NSView* v = _connectWindow.contentView;
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;

  [v addSubview:Label(@"Server:", NSMakeRect(20, 182, 100, 22))];
  _host = [NSTextField textFieldWithString:[defaults stringForKey:@"lastHost"] ?: @""];
  _host.frame = NSMakeRect(130, 180, 260, 24);
  _host.placeholderString = @"mac.local or 192.168.1.20";
  [v addSubview:_host];

  [v addSubview:Label(@"Account:", NSMakeRect(20, 148, 100, 22))];
  _user = [NSTextField textFieldWithString:[defaults stringForKey:@"lastUser"] ?: NSUserName()];
  _user.frame = NSMakeRect(130, 146, 260, 24);
  [v addSubview:_user];

  [v addSubview:Label(@"Password:", NSMakeRect(20, 114, 100, 22))];
  _password = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(130, 112, 260, 24)];
  [v addSubview:_password];

  _usePairing = [NSButton checkboxWithTitle:@"Use a pairing code instead"
                                     target:self
                                     action:@selector(pairingToggled:)];
  _usePairing.frame = NSMakeRect(128, 82, 260, 22);
  [v addSubview:_usePairing];
  _code = [NSTextField textFieldWithString:@""];
  _code.frame = NSMakeRect(130, 54, 260, 24);
  _code.placeholderString = @"8 digits shown by the server";
  _code.hidden = YES;
  [v addSubview:_code];

  _spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(130, 18, 20, 20)];
  _spinner.style = NSProgressIndicatorStyleSpinning;
  _spinner.displayedWhenStopped = NO;
  [v addSubview:_spinner];
  _connectButton = [NSButton buttonWithTitle:@"Connect" target:self action:@selector(connect:)];
  _connectButton.frame = NSMakeRect(290, 12, 106, 32);
  _connectButton.keyEquivalent = @"\r";
  [v addSubview:_connectButton];

  [_connectWindow center];
  [_connectWindow makeKeyAndOrderFront:nil];
  [_connectWindow makeFirstResponder:_host.stringValue.length ? _password : _host];
  [NSApp activateIgnoringOtherApps:YES];
}

- (void)buildMenu {
  NSMenu* bar = [[NSMenu alloc] init];
  NSMenuItem* appItem = [[NSMenuItem alloc] init];
  [bar addItem:appItem];
  NSMenu* appMenu = [[NSMenu alloc] init];
  [appMenu addItemWithTitle:@"Quit rdmachine" action:@selector(terminate:) keyEquivalent:@"q"];
  appItem.submenu = appMenu;
  NSMenuItem* editItem = [[NSMenuItem alloc] init];
  [bar addItem:editItem];
  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
  [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
  [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
  [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
  [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
  editItem.submenu = edit;
  NSApp.mainMenu = bar;
}

- (void)pairingToggled:(id)sender {
  const BOOL pairing = _usePairing.state == NSControlStateValueOn;
  _code.hidden = !pairing;
  _password.enabled = !pairing;
  _user.enabled = !pairing;
  [_connectWindow makeFirstResponder:pairing ? _code : _password];
}

- (void)setBusy:(BOOL)busy {
  _connectButton.enabled = !busy;
  _host.enabled = !busy;
  if (busy) [_spinner startAnimation:nil];
  else [_spinner stopAnimation:nil];
}

- (void)alert:(NSString*)title text:(NSString*)text {
  NSAlert* a = [[NSAlert alloc] init];
  a.messageText = title;
  a.informativeText = text;
  [a beginSheetModalForWindow:_connectWindow completionHandler:nil];
}

// Background thread: asks on the main thread whether to trust a first-time
// fingerprint.
- (BOOL)askTrust:(NSString*)host fingerprint:(NSString*)fp {
  __block BOOL yes = NO;
  dispatch_sync(dispatch_get_main_queue(), ^{
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"First connection to %@", host];
    a.informativeText = [NSString
        stringWithFormat:@"Check that this fingerprint matches the one the server shows "
                         @"(rdmachine-server --fingerprint on that Mac):\n\n%@\n\nIt will be remembered, "
                         @"and a different one will be refused from now on.",
                         fp];
    [a addButtonWithTitle:@"Trust and Connect"];
    [a addButtonWithTitle:@"Cancel"];
    yes = [a runModal] == NSAlertFirstButtonReturn;
  });
  return yes;
}

- (void)connect:(id)sender {
  if (_remote) return;
  NSString* target = [_host.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  if (target.length == 0) return;
  // "host", "host:port", "[v6]:port"
  std::string host = target.UTF8String;
  uint16_t port = rdm::kDefaultPort;
  if (!host.empty() && host[0] == '[') {
    const auto close = host.find(']');
    if (close != std::string::npos) {
      if (close + 2 < host.size() && host[close + 1] == ':') port = static_cast<uint16_t>(std::atoi(host.c_str() + close + 2));
      host = host.substr(1, close - 1);
    }
  } else if (std::count(host.begin(), host.end(), ':') == 1) {
    port = static_cast<uint16_t>(std::atoi(host.c_str() + host.find(':') + 1));
    host = host.substr(0, host.find(':'));
  }
  if (port == 0) port = rdm::kDefaultPort;

  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  [defaults setObject:target forKey:@"lastHost"];
  [defaults setObject:_user.stringValue forKey:@"lastUser"];

  rdm::RemoteSession::Options opts;
  opts.control.host = host;
  opts.control.port = port;
  opts.control.client_name = NSHost.currentHost.localizedName.UTF8String ?: "Mac";
  if (_usePairing.state == NSControlStateValueOn) {
    opts.control.auth.method = rdm::AuthMethod::kPairingCode;
    opts.control.auth.code = _code.stringValue.UTF8String;
  } else {
    opts.control.auth.username = _user.stringValue.UTF8String;
    opts.control.auth.password = _password.stringValue.UTF8String;
  }
  _password.stringValue = @"";
  _code.stringValue = @"";

  auto changed = std::make_shared<bool>(false);
  __weak AppDelegate* weakSelf = self;
  NSString* hostName = NS(host);
  opts.control.trust_server = rdm::TrustWithKnownHosts(
      _knownHosts.get(), rdm::KnownHosts::Key(host, port),
      [weakSelf, hostName](const std::string& fp) {
        AppDelegate* me = weakSelf;
        return me && [me askTrust:hostName fingerprint:NS(rdm::FormatFingerprint(fp))];
      },
      changed);

  // The window exists before the session starts: pictures may arrive while
  // Start() is still waiting for the connection to settle.
  RemoteWindowController* remote = [[RemoteWindowController alloc] initWithTitle:hostName];
  rda_viewer_config_default(&opts.viewer);
  opts.viewer.user = (__bridge void*)remote;
  opts.viewer.on_stream_info = &OnStreamInfo;
  opts.viewer.on_frame = &OnFrame;
  opts.viewer.on_cursor = &OnCursor;
  // Text copied on either Mac can be pasted on the other (both ways for now;
  // a setting comes with the preferences window).
  opts.clipboard = rdm::SystemClipboard();
  opts.log = [](const std::string& line) { NSLog(@"%@", [NSString stringWithUTF8String:line.c_str()]); };
  __weak RemoteWindowController* weakRemote = remote;
  opts.on_ended = [weakSelf, weakRemote](const std::string& reason) {
    NSString* why = NS(reason);
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf sessionEnded:weakRemote reason:why];
    });
  };

  [self setBusy:YES];
  _remote = remote;
  // Blocks copy what they capture, so the move-only pieces travel in
  // shared holders.
  auto boxed = std::make_shared<rdm::RemoteSession::Options>(std::move(opts));
  auto holder = std::make_shared<std::unique_ptr<rdm::RemoteSession>>();
  auto result = std::make_shared<rdm::ConnectResult>();
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    *holder = rdm::RemoteSession::Start(std::move(*boxed), result.get());
    dispatch_async(dispatch_get_main_queue(), ^{
      AppDelegate* me = weakSelf;
      if (me) [me connectFinished:remote session:std::move(*holder) result:*result changed:*changed];
    });
  });
}

- (void)connectFinished:(RemoteWindowController*)remote
                session:(std::unique_ptr<rdm::RemoteSession>)session
                 result:(const rdm::ConnectResult&)result
                changed:(bool)changed {
  [self setBusy:NO];
  if (!session) {
    _remote = nil;
    if (changed) {
      [self alert:@"The server's identity has changed"
             text:[NSString stringWithFormat:@"The fingerprint is not the one remembered for this server, so "
                                             @"rdmachine refused to connect. If you know why (the server was "
                                             @"reinstalled), remove its line from:\n\n%@",
                                             NS(_knownHosts->path())]];
    } else if (result.error != "untrusted_server") {
      [self alert:@"Could not connect" text:DescribeError(result)];
    }
    return;
  }
  if (remote != _remote) return;  // superseded (cannot happen today); session closes here
  [remote attachSession:std::move(session)];
  remote.window.title = [NSString stringWithFormat:@"%@ (%@)", NS(result.server.server_name), remote.window.title];
  __weak AppDelegate* weakSelf = self;
  remote.onClose = ^{
    // Called from the controller's own windowWillClose: release it on the
    // next turn of the run loop, not while it is still running.
    dispatch_async(dispatch_get_main_queue(), ^{
      AppDelegate* me = weakSelf;
      if (!me) return;
      me->_remote = nil;
      [me->_connectWindow makeKeyAndOrderFront:nil];
    });
  };
  [_connectWindow orderOut:nil];
}

- (void)sessionEnded:(RemoteWindowController*)remote reason:(NSString*)reason {
  if (!remote || remote != _remote) return;
  [remote.window close];  // ends the session and brings back the connect window
  NSString* text = [reason isEqualToString:@"server_shutdown"] ? @"The server was stopped."
                   : [reason isEqualToString:@"media_lost"]    ? @"The picture connection was lost."
                   : [reason isEqualToString:@"connection_lost"]
                       ? @"The connection to the server was lost."
                       : [NSString stringWithFormat:@"The session ended (%@).", reason];
  [self alert:@"Disconnected" text:text];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  return _remote == nil;
}

- (void)applicationWillTerminate:(NSNotification*)note {
  [_remote endSession];
}

@end

int main(int argc, char** argv) {
  @autoreleasepool {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--version") == 0) {
        std::printf("rdmachine-client %s (protocol %d, rdaccel %s, AppKit %.0f)\n", rdm::VersionString(),
                    rdm::kProtocolVersion, rda_version_string(), NSAppKitVersionNumber);
        return 0;
      }
    }
    NSApplication* app = NSApplication.sharedApplication;
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    AppDelegate* delegate = [[AppDelegate alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
