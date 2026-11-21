#include "remote_window.h"

#include <deque>
#include <iterator>

using namespace rdmclient;

namespace {

// A key handled while typing with this Mac's input method, for a server that
// judges keys as they arrive (6.6.0, docs/ime.md): kept while it may still
// come back.
struct ImeKeyRecord {
  uint64_t id = 0;
  NSEvent* event = nil;
  __weak RemoteWindowController* window = nil;
  std::set<uint32_t> held;    // that window's keys down over there, before it
  std::set<uint16_t> taken;   // keys whose key-down the input method took, before it
  uint64_t releases = 0;      // that window's releaseHeld count, before it
  // How it went, for whether it may still come back (imeAwaitsJudgement):
  bool raw_no_field = false;  // as keys, this Mac taking it that no text field takes text there
  bool im = false;            // its key-down went to the input method
  uint64_t ack = 0;           // what this Mac had heard of back and typed then
};

// A key typed while one sent earlier may still come back: it goes in after
// that is settled, in order.
struct ImeHeldKey {
  NSEvent* event = nil;
  __weak RemoteWindowController* window = nil;
  uint64_t releases = 0;  // that window's releaseHeld count when it was typed
};
// Far more than can be under way: a key comes back within a network trip.
constexpr size_t kImeKeysKept = 256;

}  // namespace

@implementation ExtraDisplayWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

namespace rdmclient {

void OnStreamInfo(void* user, int32_t w, int32_t h) {
  [(__bridge RemoteWindowController*)user streamInfo:w height:h];
}
void OnFrame(void* user, const rda_image* img, const rda_rect* changed, size_t n, const rda_frame_info*) {
  [(__bridge RemoteWindowController*)user frame:img changed:changed count:n];
}
void OnCursor(void* user, const rda_cursor_state* c) { [(__bridge RemoteWindowController*)user cursor:c]; }
void OnMessage(void* user, uint32_t type, const uint8_t* payload, size_t n) {
  if (type == rdm::kImeStateMessageType) {
    if (auto s = rdm::DecodeImeState(std::string(reinterpret_cast<const char*>(payload), n))) {
      [(__bridge RemoteWindowController*)user imeStateReport:*s];
    }
    return;
  }
  if (type != rdm::kSessionStatsMessageType) return;
  if (auto r = rdm::DecodeSessionStats(std::string(reinterpret_cast<const char*>(payload), n))) {
    [(__bridge RemoteWindowController*)user statsReport:*r];
  }
}

}  // namespace rdmclient

@implementation RemoteView {
  NSTrackingArea* _tracking;
  double _scrollX, _scrollY;  // fractions of a line not sent yet
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }

// A text field for macOS only while one over there takes text: as on that Mac,
// where nothing takes text no input method is active and no input source
// indicator shows (scrolling Claude's conversation moved its focus off the
// text field, and the indicator kept popping up here). AppKit asks again at
// the end of each pass of the event loop.
- (NSTextInputContext*)inputContext {
  return [self.owner imeOffersText] ? [super inputContext] : nil;
}

- (NSTextInputContext*)imeContext {
  return [super inputContext];
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  if (_tracking) [self removeTrackingArea:_tracking];
  _tracking = [[NSTrackingArea alloc]
      initWithRect:self.bounds
           options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                   (self.trackAlways ? NSTrackingActiveAlways : NSTrackingActiveInKeyWindow) | NSTrackingInVisibleRect
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
  // A drag that leaves the picture (past the window's edge, into the bars
  // beside it) goes on along its edge, and the button is let go there: the
  // server must see the release, or it keeps dragging.
  const BOOL dragging = kind != RDA_INPUT_MOUSE_DOWN && [self.owner buttonHeld];
  if (![self.owner streamPoint:p x:&ev.x y:&ev.y clamp:dragging]) return;
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
- (void)mouseDown:(NSEvent*)e {
  [self.owner imeMouseDown];
  [self pointer:e kind:RDA_INPUT_MOUSE_DOWN];
}
- (void)mouseUp:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_UP]; }
- (void)rightMouseDown:(NSEvent*)e {
  [self.owner imeMouseDown];
  [self pointer:e kind:RDA_INPUT_MOUSE_DOWN];
}
- (void)rightMouseUp:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_UP]; }
- (void)otherMouseDown:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_DOWN]; }
- (void)otherMouseUp:(NSEvent*)e { [self pointer:e kind:RDA_INPUT_MOUSE_UP]; }

// The wire carries whole lines (wheel notches), y > 0 up. A trackpad gives
// points; about 10 points make a line. Fractions are carried over.
- (void)scrollWheel:(NSEvent*)e {
  rda_input_event ev = Event(RDA_INPUT_SCROLL);
  const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  if (![self.owner streamPoint:p x:&ev.x y:&ev.y clamp:NO]) return;
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

// ---- NSTextInputClient: this Mac's input method (docs/ime.md) ----
// Only the composition is kept here; the text itself is the remote app's.

static NSString* PlainString(id s) { return [s isKindOfClass:NSAttributedString.class] ? [s string] : (NSString*)s; }

- (void)insertText:(id)string replacementRange:(NSRange)range {
  [self.owner imeInsert:PlainString(string) ?: @""];
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)range {
  [self.owner imeMark:PlainString(string) ?: @"" selected:sel];
}
- (void)unmarkText {
  [self.owner imeUnmark];
}
- (void)doCommandBySelector:(SEL)selector {
  [self.owner imeCommand];
}
- (BOOL)hasMarkedText {
  return [self.owner imeHasMarked];
}
- (NSRange)markedRange {
  return [self.owner imeMarkedRange];
}
- (NSRange)selectedRange {
  const NSRange m = [self.owner imeMarkedRange];
  return m.location == NSNotFound ? NSMakeRange(0, 0) : NSMakeRange(NSMaxRange(m), 0);
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
  return @[];
}
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual {
  return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
  return NSNotFound;
}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
  if (actual) *actual = range;
  return [self.owner imeCaretRect];
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
  std::string _sentInputSource;  // id last sent to the server; main thread
  // The server built a desktop for this window (docs/protocol.md, "请求桌面"):
  // the picture is the window's own pixels, and when the window changes the
  // server is asked for a desktop of the new size. Main thread.
  BOOL _desktopBuilt;
  pid_t _secureInputHolder;  // who blocks input methods (0 = nobody), as last seen
  std::optional<rdm::DesktopSpec> _desktopWanted;  // last asked for
  CALayer* _statsLayer;          // the statistics overlay's background; main thread
  CATextLayer* _statsText;       // its text
  std::optional<rdm::SessionStatsReport> _stats;
  std::chrono::steady_clock::time_point _connectedAt;
  // Input to screen: from a key, click or scroll sent on a still screen to
  // the next picture (what a user feels; rdaccel's remote_mac measures it so).
  std::atomic<uint64_t> _lastFrameUs;      // decode thread writes
  std::atomic<uint64_t> _inputPendingUs;   // 0 = none waiting
  std::atomic<double> _responseMs;         // last measurement; < 0 = none yet

  std::unique_ptr<rdm::RemoteSession> _session;  // main thread
  rda_viewer* _viewer;                           // _session's, while attached
  std::set<uint32_t> _heldKeys, _heldButtons;    // main thread
  BOOL _releasing;       // releaseHeld is sending: goes out even in observe mode
  uint64_t _releaseGen;  // counts releaseHeld (the input method's kept keys, 6.6.0)
  uint64_t _trafficBase;  // received by this window's earlier sessions (it reconnects in place)
  int32_t _lastX, _lastY;

  // Two surfaces: the decode thread fills the one not on screen with what
  // changed since it was last filled, the main thread shows it.
  std::mutex _surfMu;
  IOSurfaceRef _surf[2];
  int32_t _surfW, _surfH;
  std::vector<rda_rect> _owed[2];
  int _front;
  std::atomic<bool> _presentPending;
  NSView* _statusDim;               // darkens the frozen picture behind the card
  NSVisualEffectView* _statusView;  // the card
  NSImageView* _statusIcon;
  NSTextField* _statusTitle;
  NSTextField* _statusDetail;
  NSTextField* _statusCountdown;
  NSProgressIndicator* _statusSpinner;
  NSButton* _statusRetry;
  NSDate* _statusDeadline;
  NSTimer* _statusTimer;
  BOOL _enteringFullScreen;
  // Extended desktop. In the main window: the extra displays' windows and
  // their connections under way. In an extra display's window: which one it
  // is (-1 in the main window), whose session it uses, and whether its
  // picture is connected.
  NSMutableArray<RemoteWindowController*>* _extras;
  dispatch_group_t _extraConnects;
  NSInteger _extraIndex;
  __weak RemoteWindowController* _parent;
  BOOL _extraLive;
  // The client's input method (docs/ime.md), kept in the main window's
  // controller for all of the session's windows. Main thread.
  std::optional<rdm::ImeState> _imeState;  // the server's last word; none = it does not offer it
  // A state that came before attachSession (the picture connects before the
  // window has the session): taken over once it does.
  std::optional<rdm::ImeState> _imeEarly;
  BOOL _imeMode;                // the server is on RDMachine Input for us
  BOOL _imeModeSaid;            // this session's server heard which mode this Mac is in
  BOOL _imeOffered;             // the remote view offered macOS a text input context, as last told
  BOOL _imeServerSilent;        // no ime state came: a server older than 6.2.0
  uint64_t _imeGen;             // counts sessions, for timers that outlive one
  BOOL _imeLocalIsIM;           // this Mac's input source composes (Pinyin, ...): keys go through it
  uint64_t _imeSeq;             // the last ime message sent
  uint64_t _imeComposingSeq;    // the message that started the composition
  NSString* _imeMarked;         // the composition, nil when none
  NSEvent* _imeEvent;           // the key the input method is handling now
  BOOL _imeEventRaw;            // ... and it said the key should go as a key
  std::set<uint16_t> _imeTaken;  // keys whose key-down the input method took (their key-up goes nowhere)
  __weak RemoteView* _imeView;  // where the input method works now
  NSRect _imeLastCaret;         // screen coordinates; empty = none yet
  NSPoint _imeLastClick;        // screen point of the last click in the picture; valid if _imeClicked
  BOOL _imeClicked;
  uint32_t _imeField;           // the server's field number _imeLastCaret belongs to (0 = not said)
  std::string _imeLastApp;      // ... or, from a server that does not say, the last app named
  // This Mac's input method may answer "drop the composition" by putting it
  // in: that commit is not sent (it was sent already, or dropped on purpose),
  // until the next key goes to the input method.
  BOOL _imeDropInsert;
  // Where the caret will be once the text just committed is in, until the
  // server says (a round trip: the next composition starts before that).
  // Stream pixels; the committed text's width is guessed from its width in
  // the system font, scaled by what the last commits really took (6.3.6).
  std::optional<rdm::ImeRect> _imeAhead;
  uint64_t _imeAheadSeq;        // the commit it waits for
  int32_t _imeAheadFromX;       // the caret the server last gave, before those commits
  double _imeAheadUnits;        // their width at 100 pt
  double _imePixelsPerUnit;     // learned; 0 = not yet
  // A server that judges keys as they arrive (6.6.0, docs/ime.md): each key
  // handled while typing with this Mac's input method goes over numbered,
  // with what the input method said for it, and is kept a while. The server
  // may give it back (a text field took input methods there when it came:
  // handled again here), or say the composition went nowhere (no field took
  // it there: the keys were typed as keys).
  uint64_t _imeKeyId;                        // the last key's number
  uint64_t _imeBackSeen, _imeTypedSeen;      // the server's back and typed, as last heard
  uint64_t _imeAck;                          // the larger: what the server knows this Mac heard
  BOOL _imeCollecting;                       // a key is being handled: what the input method says goes with it
  std::vector<rdm::ImeInput> _imeCollected;
  BOOL _imeCapture;                          // ... and so do the keys it sends as keys (send:)
  std::vector<rda_input_event> _imeCaptured;
  std::deque<ImeKeyRecord> _imeKeys;         // oldest first
  uint64_t _imeJudgedSeen;                   // the server's judged, as last heard
  // A key sent before this Mac heard of a text field over there may still
  // come back: the keys typed meanwhile wait, to go in after it.
  BOOL _imeHolding;
  std::vector<ImeHeldKey> _imeHeld;
}

- (instancetype)initWithTitle:(NSString*)title {
  if (!(self = [super init])) return nil;
  _front = -1;
  _extraIndex = -1;
  _extras = [NSMutableArray array];
  _extraConnects = dispatch_group_create();
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 1280, 800)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = title;
  _window.releasedWhenClosed = NO;
  _window.animationBehavior = NSWindowAnimationBehaviorNone;  // as the connect window
  _window.delegate = self;
  _window.acceptsMouseMovedEvents = YES;
  _view = [[RemoteView alloc] initWithFrame:_window.contentView.bounds];
  _view.owner = self;
  _view.wantsLayer = YES;
  _window.contentView = _view;
  _layer = _view.layer;
  _layer.backgroundColor = NSColor.blackColor.CGColor;
  _layer.contentsGravity = kCAGravityResizeAspect;
  // A layer, not a window or view: it takes no mouse events, so clicks reach
  // the remote screen under it.
  _lastFrameUs = 0;
  _inputPendingUs = 0;
  _responseMs = -1;
  _statsLayer = [CALayer layer];
  _statsLayer.hidden = YES;
  _statsLayer.zPosition = 20;
  _statsLayer.cornerRadius = 6;
  _statsText = [CATextLayer layer];
  _statsText.wrapped = NO;
  [_statsLayer addSublayer:_statsText];
  _cursorLayer = [CALayer layer];
  _cursorLayer.anchorPoint = CGPointZero;
  _cursorLayer.hidden = YES;
  [_layer addSublayer:_cursorLayer];
  [_layer addSublayer:_statsLayer];
  return self;
}

- (instancetype)initExtraFor:(RemoteWindowController*)parent index:(NSUInteger)index screen:(NSScreen*)screen {
  if (!(self = [self initWithTitle:@""])) return nil;
  _extraIndex = static_cast<NSInteger>(index);
  _parent = parent;
  // The whole screen, its menu bar included, above everything of this Mac;
  // out of the way while another application is in front.
  ExtraDisplayWindow* w = [[ExtraDisplayWindow alloc] initWithContentRect:screen.frame
                                                                styleMask:NSWindowStyleMaskBorderless
                                                                  backing:NSBackingStoreBuffered
                                                                    defer:NO];
  w.releasedWhenClosed = NO;
  w.animationBehavior = NSWindowAnimationBehaviorNone;
  w.level = NSMainMenuWindowLevel + 1;
  w.hidesOnDeactivate = YES;
  w.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorMoveToActiveSpace;
  w.backgroundColor = NSColor.blackColor;
  w.acceptsMouseMovedEvents = YES;
  w.delegate = self;
  [_window setDelegate:nil];
  _view.trackAlways = YES;
  _view.frame = NSMakeRect(0, 0, screen.frame.size.width, screen.frame.size.height);
  w.contentView = _view;
  [w setFrame:screen.frame display:NO];
  _window = w;
  // Keys typed while this window is the key one go to the server through it.
  __weak RemoteWindowController* weakSelf = self;
  _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp |
                                                              NSEventMaskFlagsChanged
                                                      handler:^NSEvent*(NSEvent* e) {
                                                        RemoteWindowController* me = weakSelf;
                                                        if (!me || e.window != me.window) return e;
                                                        if ([me localShortcut:e]) return nil;
                                                        if (!gObserveOnly) {
                                                          [me key:e];
                                                          return nil;
                                                        }
                                                        // Observing: nothing goes to the server; ⌘ shortcuts
                                                        // work in this app's own menus instead.
                                                        return (e.modifierFlags & NSEventModifierFlagCommand) ? e : nil;
                                                      }];
  return self;
}

- (BOOL)wantsFullScreen {
  return (_window.styleMask & NSWindowStyleMaskFullScreen) != 0 || _enteringFullScreen;
}

- (NSScreen*)homeScreen {
  return _window.screen ?: NSScreen.mainScreen;
}

- (void)sendExtra:(NSUInteger)index event:(const rda_input_event&)ev {
  if (_session) _session->SendExtraInput(index, ev);
}

- (void)extraConnected {
  _extraLive = YES;
}

- (void)toggleFullScreenFromExtra {
  [self releaseHeld];
  [_window toggleFullScreen:nil];
}

// Main window: a window for each extra display the server built, on the
// screen it stands for, and its picture connection (in the background).
- (void)startExtras {
  if (_extraIndex >= 0 || !_session) return;
  const rdm::SessionReady ready = _session->session();
  NSScreen* home = [self homeScreen];
  rdm::RemoteSession* session = _session.get();  // outlives the connects: endSession waits for them
  for (size_t i = 0; i < ready.extra_displays.size(); ++i) {
    const rdm::ExtraDisplay& x = ready.extra_displays[i];
    NSScreen* screen = ScreenAt(home, x.x, x.y);
    if (!screen) {
      NSLog(@"extra display %zu: no screen here at %d,%d any more", i + 2, x.x, x.y);
      continue;
    }
    RemoteWindowController* w = [[RemoteWindowController alloc] initExtraFor:self index:i screen:screen];
    [w.window orderFront:nil];
    [_extras addObject:w];
    dispatch_group_async(_extraConnects, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      rda_viewer_config c;
      rda_viewer_config_default(&c);
      c.user = (__bridge void*)w;  // w is kept by this block and _extras until the viewer is gone
      c.on_stream_info = &OnStreamInfo;
      c.on_frame = &OnFrame;
      c.on_cursor = &OnCursor;
      std::string error;
      const bool ok = session->ConnectExtra(i, c, &error);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (ok) {
          [w extraConnected];
        } else {
          NSLog(@"extra display %zu: %s", i + 2, error.c_str());
        }
      });
    });
  }
  if (_extras.count > 0) _view.trackAlways = YES;
  [_view updateTrackingAreas];
}

// Main window: the extra displays' windows go (their pictures are closed
// already: by the session's Close or a move).
- (void)closeExtras {
  for (RemoteWindowController* w in _extras) {
    [w endSession];
    w.window.delegate = nil;
    [w.window orderOut:nil];
  }
  [_extras removeAllObjects];
  _view.trackAlways = NO;
  [_view updateTrackingAreas];
}

// Full screen entered or left, or the setting changed: connect again if the
// layout of displays should change.
- (void)checkDisplays {
  if (_extraIndex >= 0 || !_session) return;
  const BOOL wanted = UseAllDisplays() && [self wantsFullScreen] && NSScreen.screens.count > 1;
  if (wanted != self.extrasRequested && self.onDisplaysChanged) self.onDisplaysChanged();
}

// The window as it was left last time: the whole screen if it was full
// screen, else its last size (within the screen), else 85% of the screen.
// The window is then sized to the stream (streamSizeChanged).
- (rdm::DesktopSpec)initialDesktop {
  NSScreen* screen = _window.screen ?: NSScreen.mainScreen;
  const NSSize room = screen.visibleFrame.size;
  const double scale = DesktopScale(screen.backingScaleFactor);
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  NSSize want = NSMakeSize(room.width * 0.85, room.height * 0.85);
  if ([defaults boolForKey:kRemoteFullScreenKey]) {
    want = screen.frame.size;  // full screen hides this Mac's menu bar and Dock
  } else if (NSString* saved = [defaults stringForKey:kRemoteSizeKey]) {
    const NSSize s = NSSizeFromString(saved);
    if (s.width >= rdm::kMinDesktopPoints && s.height >= rdm::kMinDesktopPoints) {
      want = NSMakeSize(std::min(s.width, room.width), std::min(s.height, room.height));
    }
  }
  rdm::DesktopSpec d;
  d.width = static_cast<int32_t>(std::floor(want.width) * scale);
  d.height = static_cast<int32_t>(std::floor(want.height) * scale);
  d.scale = scale;
  return d;
}

- (BOOL)isLive {
  return _session != nullptr;
}

- (NSString*)liveDetail {
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - _connectedAt).count();
  NSMutableArray* parts = [NSMutableArray arrayWithObject:[@"已连接 " stringByAppendingString:DurationText(secs)]];
  if (_stats) {
    if (_stats->rtt_ms > 0) [parts addObject:[NSString stringWithFormat:@"%.0f ms", _stats->rtt_ms]];
    if (_stats->width > 0 && _stats->height > 0) {
      [parts addObject:[NSString stringWithFormat:@"%d×%d", _stats->width, _stats->height]];
    }
  }
  return [parts componentsJoinedByString:@" · "];
}

// A card over the picture while it is not live: what happened, what it is
// doing about it (and until when), and the way out.
- (void)showStatus:(NSString*)title
            detail:(NSString*)detail
            symbol:(NSString*)symbol
             until:(NSDate*)deadline
          canRetry:(BOOL)canRetry {
  if (_closed) return;  // a late callback of a session that has ended
  if (!_statusView) {
    _statusDim = [[NSView alloc] initWithFrame:_view.bounds];
    _statusDim.wantsLayer = YES;
    _statusDim.layer.backgroundColor = [NSColor colorWithWhite:0 alpha:0.45].CGColor;
    _statusDim.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    _statusView = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, 420, 148)];
    _statusView.material = NSVisualEffectMaterialHUDWindow;
    _statusView.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    _statusView.state = NSVisualEffectStateActive;
    _statusView.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    _statusView.wantsLayer = YES;
    _statusView.layer.cornerRadius = 14;
    _statusView.layer.borderWidth = 0.5;
    _statusView.layer.borderColor = [NSColor colorWithWhite:1 alpha:0.12].CGColor;
    _statusView.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin | NSViewMaxYMargin;

    _statusIcon = [[NSImageView alloc] initWithFrame:NSMakeRect(22, 86, 36, 36)];
    _statusIcon.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:26 weight:NSFontWeightMedium];
    _statusIcon.contentTintColor = NSColor.systemOrangeColor;
    [_statusView addSubview:_statusIcon];
    _statusTitle = [NSTextField labelWithString:@""];
    _statusTitle.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    _statusTitle.textColor = NSColor.labelColor;
    _statusTitle.frame = NSMakeRect(72, 102, 330, 20);
    _statusTitle.lineBreakMode = NSLineBreakByTruncatingTail;
    [_statusView addSubview:_statusTitle];
    _statusDetail = [NSTextField wrappingLabelWithString:@""];
    _statusDetail.font = [NSFont systemFontOfSize:12];
    _statusDetail.textColor = NSColor.secondaryLabelColor;
    _statusDetail.frame = NSMakeRect(72, 64, 330, 34);
    [_statusView addSubview:_statusDetail];

    _statusSpinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(22, 20, 16, 16)];
    _statusSpinner.style = NSProgressIndicatorStyleSpinning;
    _statusSpinner.controlSize = NSControlSizeSmall;
    _statusSpinner.displayedWhenStopped = NO;
    [_statusView addSubview:_statusSpinner];
    _statusCountdown = [NSTextField labelWithString:@""];
    _statusCountdown.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
    _statusCountdown.textColor = NSColor.tertiaryLabelColor;
    _statusCountdown.frame = NSMakeRect(44, 20, 160, 16);
    [_statusView addSubview:_statusCountdown];

    NSButton* stop = [NSButton buttonWithTitle:@"断开" target:_window action:@selector(performClose:)];
    stop.frame = NSMakeRect(318, 12, 88, 30);
    [_statusView addSubview:stop];
    _statusRetry = [NSButton buttonWithTitle:@"立即重试" target:self action:@selector(retryNow:)];
    _statusRetry.frame = NSMakeRect(222, 12, 92, 30);
    [_statusView addSubview:_statusRetry];
  }
  _statusIcon.image = [NSImage imageWithSystemSymbolName:symbol ?: @"wifi.exclamationmark" accessibilityDescription:nil];
  _statusTitle.stringValue = title ?: @"";
  _statusDetail.stringValue = detail ?: @"";
  _statusDeadline = deadline;
  _statusRetry.hidden = !canRetry;
  [_statusSpinner startAnimation:nil];
  [self updateStatusCountdown];
  [_statusTimer invalidate];
  // Weak: a repeating timer with a target keeps the target, and with it the
  // window and its last picture, alive for as long as it runs.
  __weak RemoteWindowController* weakSelf = self;
  _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer* t) {
    RemoteWindowController* me = weakSelf;
    if (me) [me updateStatusCountdown];
    else [t invalidate];
  }];
  const NSSize b = _view.bounds.size;
  _statusDim.frame = _view.bounds;
  [_statusView setFrameOrigin:NSMakePoint(std::round((b.width - 420) / 2), std::round((b.height - 148) / 2))];
  if (!_statusDim.superview) [_view addSubview:_statusDim];
  if (!_statusView.superview) [_view addSubview:_statusView];
}

- (void)updateStatusCountdown {
  if (!_statusDeadline) {
    _statusCountdown.stringValue = @"等待中…";
    return;
  }
  const long left = std::lround(std::max(0.0, _statusDeadline.timeIntervalSinceNow));
  _statusCountdown.stringValue = left > 0 ? [NSString stringWithFormat:@"最多再等 %ld 秒", left] : @"马上放弃…";
}

- (void)retryNow:(id)sender {
  if (self.onRetry) self.onRetry();
}

- (void)hideStatus {
  [_statusTimer invalidate];
  _statusTimer = nil;
  [_statusSpinner stopAnimation:nil];
  [_statusView removeFromSuperview];
  [_statusDim removeFromSuperview];
}

- (void)saveWindowMode {
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  const BOOL full = (_window.styleMask & NSWindowStyleMaskFullScreen) != 0;
  [defaults setBool:full forKey:kRemoteFullScreenKey];
  if (!full) [defaults setObject:NSStringFromSize(_view.bounds.size) forKey:kRemoteSizeKey];
}

// The picture area in pixels, as a desktop request.
- (rdm::DesktopSpec)viewDesktop {
  const double scale = DesktopScale(_window.backingScaleFactor);
  const NSSize b = _view.bounds.size;
  rdm::DesktopSpec d;
  d.width = static_cast<int32_t>(std::lround(b.width * scale));
  d.height = static_cast<int32_t>(std::lround(b.height * scale));
  d.scale = scale;
  return d;
}

// Main thread. The window's size or screen changed: once it has settled,
// ask the server for a desktop of the new size. Nothing is sent while the
// window is being dragged (every step would rebuild the server's display).
- (void)desktopNeedsUpdate {
  [self updatePictureFilter];
  if (!_desktopBuilt) return;
  [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(requestDesktop) object:nil];
  [self performSelector:@selector(requestDesktop) withObject:nil afterDelay:0.25];
}

// Main thread. A picture shown at a whole multiple of its size (a 1x
// desktop on a Retina screen: text mode) gets every pixel repeated, which
// keeps the text's edges as the server drew them; smoothing a 2x
// enlargement blurs every glyph. Any other ratio is smoothed as before.
- (void)updatePictureFilter {
  const double backing = _window.backingScaleFactor > 0 ? _window.backingScaleFactor : 1;
  const double ratio = _streamW > 0 ? _view.bounds.size.width * backing / _streamW : 1;
  const bool whole = ratio > 1.5 && std::abs(ratio - std::round(ratio)) < 0.01;
  CALayerContentsFilter filter = whole ? kCAFilterNearest : kCAFilterLinear;
  if (![_layer.magnificationFilter isEqualToString:filter]) {
    _layer.magnificationFilter = filter;
    NSLog(@"picture %dx%d in a view of %.0fx%.0f points at %.0fx: %@", _streamW, _streamH, _view.bounds.size.width,
          _view.bounds.size.height, backing, whole ? @"pixels repeated" : @"smoothed");
  }
}

- (void)requestDesktop {
  if (!_desktopBuilt || !_viewer || _window.inLiveResize) return;
  const std::optional<rdm::DesktopSpec> wanted = rdm::PlanDesktop([self viewDesktop]);
  if (!wanted || wanted == _desktopWanted) return;
  const std::string payload = rdm::EncodeDesktopSpec(*wanted);
  if (_session->SendMessage(rdm::kDesktopRequestMessageType,
                              reinterpret_cast<const uint8_t*>(payload.data()), payload.size()) == RDA_OK) {
    _desktopWanted = wanted;
  }
}

- (void)attachSession:(std::unique_ptr<rdm::RemoteSession>)session activate:(BOOL)activate {
  if (_session) [self endSession];  // never two at once: monitors and observers are per session
  [self hideStatus];
  _session = std::move(session);
  _viewer = _session->viewer();
  _desktopWanted = _session->session().desktop;
  _desktopBuilt = _desktopWanted.has_value();
  if (_desktopBuilt) {
    // Any shape goes: the desktop follows the window. (Without a desktop the
    // window keeps the server display's proportions, see streamSizeChanged.)
    _window.contentResizeIncrements = NSMakeSize(1, 1);
    _window.contentMinSize = NSMakeSize(rdm::kMinDesktopPoints, rdm::kMinDesktopPoints);
    [self desktopNeedsUpdate];  // the window may not be the size that was asked for
  }
  if (activate) [_window makeKeyAndOrderFront:nil];
  [_window makeFirstResponder:_view];
  // Input source sync: whatever switches it here (Caps Lock, ⌃Space, the
  // menu) posts this distributed notification.
  [NSDistributedNotificationCenter.defaultCenter addObserver:self
                                                    selector:@selector(inputSourceChanged:)
                                                        name:(__bridge NSString*)kTISNotifySelectedKeyboardInputSourceChanged
                                                      object:nil
                                          suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(syncSettingChanged:)
                                             name:kSyncKeyboardLanguageChanged
                                           object:nil];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(imeUpdateMode)
                                             name:kUseLocalInputMethodChanged
                                           object:nil];
  [self imeReset];  // a new session: the server says again whether it offers it
  if (_imeEarly) {
    // It said so before this window had the session.
    const rdm::ImeState early = *_imeEarly;
    _imeEarly.reset();
    [self imeStateReport:early];
  }
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(sendPictureMode)
                                             name:kTextModeChanged
                                           object:nil];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(playSoundChanged)
                                             name:kPlaySoundChanged
                                           object:nil];
  if (!PlaySound()) _session->SetAudioPaused(true);  // turned off while connecting
  [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(observeChanged) name:kObserveChanged object:nil];
  [self sendInputSource:YES];
  // A server older than 6.2.0 never says whether it takes this Mac's input
  // method: typing with it then follows the keyboard language, as before.
  {
    __weak RemoteWindowController* weakSelf = self;
    const uint64_t gen = _imeGen;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      RemoteWindowController* me = weakSelf;
      if (!me || me->_imeGen != gen || !me->_session || me->_imeState) return;
      me->_imeServerSilent = YES;
      [me sendInputSource:YES];
    });
  }
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(checkDisplays)
                                             name:kUseAllDisplaysChanged
                                           object:nil];
  [self startExtras];
  _connectedAt = std::chrono::steady_clock::now();
  [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(renderStats) name:kStatsChanged object:nil];
  // Keys go through a local monitor rather than keyDown: it also sees
  // Command shortcuts (and their key-up, which AppKit otherwise swallows).
  __weak RemoteWindowController* weakSelf = self;
  _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp |
                                                              NSEventMaskFlagsChanged
                                                      handler:^NSEvent*(NSEvent* e) {
                                                        RemoteWindowController* me = weakSelf;
                                                        if (!me || e.window != me.window) return e;
                                                        if ([me localShortcut:e]) return nil;
                                                        if (!gObserveOnly) {
                                                          [me key:e];
                                                          return nil;
                                                        }
                                                        // Observing: nothing goes to the server; ⌘ shortcuts
                                                        // work in this app's own menus instead.
                                                        return (e.modifierFlags & NSEventModifierFlagCommand) ? e : nil;
                                                      }];
}

// Main thread. The server moved the session to another graphical session of
// that Mac (someone logged in at its login window, say): a new picture
// connection, which has to hear again what this window wants.
- (void)sessionMoved:(const rdm::SessionReady&)ready {
  if (!_session) return;
  [self hideStatus];
  [self releaseHeld];  // the new session never saw those keys go down
  [self imeReset];     // nor the composition; its server says again what it offers
  _desktopWanted = ready.desktop;
  _desktopBuilt = ready.desktop.has_value();
  if (_desktopBuilt) {
    _window.contentResizeIncrements = NSMakeSize(1, 1);
    _window.contentMinSize = NSMakeSize(rdm::kMinDesktopPoints, rdm::kMinDesktopPoints);
    [self desktopNeedsUpdate];
  }
  [self sendInputSource:YES];
  [self sendPictureMode];
  [self closeExtras];  // the old desktop's displays are gone; the new one's:
  [self startExtras];
}

// Main thread. Lets go of held keys, says bye, frees everything. No viewer
// callback runs after it returns.
- (void)endSession {
  [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(requestDesktop) object:nil];
  _desktopBuilt = NO;
  [NSDistributedNotificationCenter.defaultCenter removeObserver:self];
  [NSNotificationCenter.defaultCenter removeObserver:self name:kSyncKeyboardLanguageChanged object:nil];
  [NSNotificationCenter.defaultCenter removeObserver:self name:kTextModeChanged object:nil];
  [NSNotificationCenter.defaultCenter removeObserver:self name:kPlaySoundChanged object:nil];
  [NSNotificationCenter.defaultCenter removeObserver:self name:kObserveChanged object:nil];
  [NSNotificationCenter.defaultCenter removeObserver:self name:kUseLocalInputMethodChanged object:nil];
  [self imeReset];
  [NSNotificationCenter.defaultCenter removeObserver:self name:kStatsChanged object:nil];
  [NSNotificationCenter.defaultCenter removeObserver:self name:kUseAllDisplaysChanged object:nil];
  if (_keyMonitor) {
    [NSEvent removeMonitor:_keyMonitor];
    _keyMonitor = nil;
  }
  [self releaseHeld];
  _viewer = nullptr;
  _extraLive = NO;
  if (_session) {
    _session->Close();  // closes the extra displays' pictures too
    _trafficBase += _session->BytesReceived();
    // Connects of extra displays still under way end at once (Close stops
    // them); wait for them before the session goes.
    dispatch_group_wait(_extraConnects, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    _session.reset();
  }
  if (_extraIndex < 0) [self closeExtras];
  if (_cursorImage) CGImageRelease(_cursorImage);
  _cursorImage = nullptr;
  std::lock_guard<std::mutex> lock(_surfMu);
  for (auto& s : _surf) {
    if (s) CFRelease(s);
    s = nullptr;
  }
}

- (void)dealloc {
  [_statusTimer invalidate];
  [self endSession];
}

- (void)windowWillClose:(NSNotification*)note {
  _closed = YES;
  [self saveWindowMode];
  [self hideStatus];  // and its timer
  [self endSession];
  if (self.onClose) self.onClose();
}

// ⌃⌥⌘F stays on this Mac and toggles full screen, ⌃⌥⌘O switches between
// observing and controlling; every other key goes to the server. Full screen hides this Mac's menu bar and Dock completely, so
// this is the way out.
- (BOOL)localShortcut:(NSEvent*)e {
  if (e.type != NSEventTypeKeyDown && e.type != NSEventTypeKeyUp) return NO;
  const NSEventModifierFlags want = NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagCommand;
  const NSEventModifierFlags mods =
      e.modifierFlags & (want | NSEventModifierFlagShift);
  if (mods != want || (e.keyCode != 3 /* kVK_ANSI_F */ && e.keyCode != 31 /* kVK_ANSI_O */)) return NO;
  if (e.keyCode == 31) {
    // ⌃⌥⌘O: observe only, or control again (the app delegate's menu item).
    if (e.type == NSEventTypeKeyDown && !e.isARepeat) [NSApp sendAction:@selector(toggleObserveOnly:) to:nil from:nil];
    return YES;
  }
  if (e.type == NSEventTypeKeyDown && !e.isARepeat) {
    [self releaseHeld];  // the server saw ⌃⌥⌘ go down; let them go there
    if (_extraIndex >= 0) {
      [_parent toggleFullScreenFromExtra];
    } else {
      [_window toggleFullScreen:nil];
    }
  }
  return YES;
}

// Full screen: hide this Mac's menu bar and Dock outright instead of letting
// them slide in at the screen's edges, where they would cover the server's
// menu bar and Dock and take the clicks meant for them.
- (NSApplicationPresentationOptions)window:(NSWindow*)window
      willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)proposed {
  proposed &= ~(NSApplicationPresentationAutoHideMenuBar | NSApplicationPresentationAutoHideDock);
  return proposed | NSApplicationPresentationFullScreen | NSApplicationPresentationHideDock |
         NSApplicationPresentationHideMenuBar;
}

- (void)enterFullScreen {
  if (_window.styleMask & NSWindowStyleMaskFullScreen) return;
  _enteringFullScreen = YES;  // from now: the first picture must not size the window
  [_window toggleFullScreen:nil];
}

- (void)windowWillEnterFullScreen:(NSNotification*)note {
  _enteringFullScreen = YES;
}

- (void)windowDidFailToEnterFullScreen:(NSWindow*)window {
  _enteringFullScreen = NO;
}

- (void)windowDidEnterFullScreen:(NSNotification*)note {
  _enteringFullScreen = NO;
  [self releaseHeld];
  [self desktopNeedsUpdate];
  [self checkDisplays];
  CATextLayer* hint = [CATextLayer layer];
  hint.string = @"按 ⌃⌥⌘F 退出全屏";
  hint.font = (__bridge CFTypeRef)[NSFont systemFontOfSize:15 weight:NSFontWeightMedium];
  hint.fontSize = 15;
  hint.alignmentMode = kCAAlignmentCenter;
  hint.foregroundColor = NSColor.whiteColor.CGColor;
  hint.backgroundColor = [NSColor colorWithWhite:0 alpha:0.7].CGColor;
  hint.cornerRadius = 8;
  hint.contentsScale = _window.backingScaleFactor;
  const CGSize size = CGSizeMake(300, 34);
  const CGRect b = _layer.bounds;
  hint.frame = CGRectMake((b.size.width - size.width) / 2, b.size.height - size.height - 40, size.width, size.height);
  hint.zPosition = 10;
  [_layer addSublayer:hint];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    [CATransaction begin];
    [CATransaction setCompletionBlock:^{
      [hint removeFromSuperlayer];
    }];
    hint.opacity = 0;
    [CATransaction commit];
  });
}

- (void)windowDidExitFullScreen:(NSNotification*)note {
  [self releaseHeld];
  [self desktopNeedsUpdate];
  [self checkDisplays];
}

- (void)windowDidEndLiveResize:(NSNotification*)note {
  [self desktopNeedsUpdate];
}

// Moved to a screen of another scale (Retina to an external 1x display):
// the same window is now another number of pixels.
- (void)windowDidChangeBackingProperties:(NSNotification*)note {
  [self desktopNeedsUpdate];
}

// ---- the client's input method (docs/ime.md) ------------------------------

// The session's state lives in the main window's controller.
- (RemoteWindowController*)imeRoot {
  return _extraIndex >= 0 && _parent ? _parent : self;
}

// Forgets the session's input method state (no message: the session it
// belonged to is gone or new).
- (void)imeReset {
  if (_imeMarked) [[_imeView imeContext] discardMarkedText];
  _imeState.reset();
  _imeMode = NO;
  _imeMarked = nil;
  _imeEvent = nil;
  _imeTaken.clear();
  _imeLastCaret = NSZeroRect;
  _imeModeSaid = NO;
  _imeServerSilent = NO;
  ++_imeGen;
  _imeClicked = NO;
  _imeField = 0;
  _imeLastApp.clear();
  _imeDropInsert = NO;
  _imeAhead.reset();
  _imePixelsPerUnit = 0;
  _imeKeyId = 0;  // the server numbers them from 1 again too
  _imeBackSeen = _imeTypedSeen = _imeAck = 0;
  _imeCollecting = _imeCapture = NO;
  _imeCollected.clear();
  _imeCaptured.clear();
  _imeKeys.clear();
  _imeJudgedSeen = 0;
  _imeHolding = NO;
  _imeHeld.clear();
  [self imeOfferChanged];  // no session's field any more: not a text field for macOS
}

- (void)imeSend:(rdm::ImeInput)m {
  if (!_session) return;
  m.ack = _imeAck;
  if (_imeCollecting && (m.kind == rdm::ImeInput::Kind::kMark || m.kind == rdm::ImeInput::Kind::kCommit ||
                         m.kind == rdm::ImeInput::Kind::kCancel)) {
    _imeCollected.push_back(std::move(m));  // goes over with the key (imeKey:from:)
    return;
  }
  const std::string p = rdm::EncodeImeInput(m);
  _session->SendInputMessage(rdm::kImeMessageType, reinterpret_cast<const uint8_t*>(p.data()), p.size());
}

// Whether keys should go to this Mac's input method now, and telling the
// server when that changes.
- (void)imeUpdateMode {
  if (_extraIndex >= 0) return;
  rda_input_source_info cur{};
  cur.struct_size = sizeof(cur);
  const BOOL was_im = _imeLocalIsIM;
  _imeLocalIsIM = rda_current_input_source(&cur) == RDA_OK && cur.input_method != 0;
  if (was_im && !_imeLocalIsIM && _imeMarked) [self imeUnmark];  // switched away mid-composition
  // The server stays on RDMachine Input whatever this Mac is on: with ABC
  // here the keys go as keys and pass through it as ABC there. Only the
  // setting and what the server can do decide it: a password field or
  // observing does not, as each change of the mode switches the server's
  // input source (6.5.0).
  const BOOL want = UseLocalInputMethod() && _viewer && _imeState && _imeState->available;
  if (_imeMarked && (!want || !_imeState->client || _imeState->secure)) {
    _imeDropInsert = YES;
    [[_imeView imeContext] discardMarkedText];
    [self imeSend:{rdm::ImeInput::Kind::kCancel, ++_imeSeq}];
    _imeMarked = nil;
  }
  // Said once per session even when it is off: a server an earlier session
  // left on RDMachine Input puts the input source from before back.
  if (want == _imeMode && (_imeModeSaid || !_imeState)) {
    [self imeOfferChanged];
    if (_imeHolding && ![self imeJudged]) [self imeLetHeldGo];
    return;
  }
  _imeModeSaid = YES;
  _imeMode = want;
  _imeTaken.clear();
  rdm::ImeInput m{rdm::ImeInput::Kind::kMode, ++_imeSeq};
  m.mode_on = want;
  [self imeSend:m];
  NSLog(@"this Mac's input method: %@", want ? @"on (the server is on RDMachine Input)" : @"off");
  [self renderStats];
  if (!want) {
    _sentInputSource.clear();
    [self sendInputSource:YES];  // the server follows this Mac's input source again
  }
  [self imeOfferChanged];
  if (_imeHolding && ![self imeJudged]) [self imeLetHeldGo];
}

// From the key monitor (any of the session's windows): YES when the input
// method took the key, or it is the key-up of one it took.
- (BOOL)imeTakes:(NSEvent*)e from:(RemoteWindowController*)window {
  if (!_imeMode || (e.type != NSEventTypeKeyDown && e.type != NSEventTypeKeyUp)) return NO;
  if (gObserveOnly) return NO;  // observing: nothing is typed over there (rawKey sends nothing either)
  // Keys go as keys where nothing over there takes text, or in a password
  // field (secure input), as with a keyboard on that Mac: no input method
  // composes there. The server stays on RDMachine Input, which passes them
  // through (6.5.3).
  if (e.type == NSEventTypeKeyDown && (!_imeState || !_imeState->client || _imeState->secure)) return NO;
  if (!_imeLocalIsIM && e.type == NSEventTypeKeyDown) return NO;  // ABC here: a key as a key
  if (e.type == NSEventTypeKeyUp) {
    if (!_imeTaken.count(e.keyCode)) return NO;
    _imeTaken.erase(e.keyCode);
    return YES;
  }
  // Shortcuts go as keys, as they do past an input method on this Mac; a
  // composition under way goes in first.
  if (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) {
    if (_imeMarked) [self imeUnmark];
    return NO;
  }
  RemoteView* view = window->_view;
  if (_imeView && _imeView != view && _imeMarked) [self imeUnmark];
  _imeView = view;
  _imeDropInsert = NO;  // what the input method puts in from here on is this key's
  _imeEvent = e;
  _imeEventRaw = NO;
  const BOOL handled = [[view imeContext] handleEvent:e];
  _imeEvent = nil;
  if (!handled || _imeEventRaw) return NO;  // a key as a key (arrows, Return, Delete when not composing)
  _imeTaken.insert(e.keyCode);
  return YES;
}

// Keys go over numbered, for the server to judge as they arrive (6.6.0).
- (BOOL)imeJudged {
  return _imeMode && _imeState && _imeState->keys && _session;
}

// A key, numbered: how this Mac handled it, by what the server last said
// (imeTakes, rawKey), goes over in one message with what its input method
// said meanwhile, for the server to judge as it arrives (docs/ime.md).
- (void)imeKey:(NSEvent*)e from:(RemoteWindowController*)window {
  if (_imeHolding) {
    // One sent before this Mac heard of the text field over there may still
    // come back: this one goes in after it, as on that Mac.
    _imeHeld.push_back({e, window, window->_releaseGen});
    return;
  }
  const uint64_t key_id = ++_imeKeyId;
  ImeKeyRecord rec;
  rec.id = key_id;
  rec.event = e;
  rec.window = window;
  rec.held = window->_heldKeys;
  rec.taken = _imeTaken;
  rec.releases = window->_releaseGen;
  _imeKeys.push_back(std::move(rec));
  while (_imeKeys.size() > kImeKeysKept) _imeKeys.pop_front();
  const BOOL down = e.type == NSEventTypeKeyDown;
  // A key-down whose way goes by whether a text field over there takes text
  // (imeTakes): what this Mac takes that to be.
  std::optional<bool> field;
  if (down && _imeLocalIsIM && !(e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) {
    field = _imeState && _imeState->client && !_imeState->secure;
  }
  _imeCollecting = YES;
  _imeCollected.clear();
  const BOOL took = [self imeTakes:e from:window];
  _imeCollecting = NO;
  rdm::ImeInput k;
  k.kind = rdm::ImeInput::Kind::kKey;
  k.key = key_id;
  k.im = took;
  k.field = took ? std::optional<bool>(true) : field;
  if (took) {
    if (!down) return;  // the key-up of one the input method took: nothing goes
    rda_input_event ev;
    if ([window keyEventOf:e into:&ev]) k.events.push_back({static_cast<uint32_t>(ev.kind), ev.code, ev.modifiers});
  } else {
    _imeCapture = YES;
    _imeCaptured.clear();
    [window rawKey:e];
    _imeCapture = NO;
    for (const rda_input_event& ev : _imeCaptured) {
      k.events.push_back({static_cast<uint32_t>(ev.kind), ev.code, ev.modifiers});
    }
    _imeCaptured.clear();
  }
  k.then = std::move(_imeCollected);
  _imeCollected.clear();
  if (!k.im && k.events.empty() && k.then.empty()) return;  // nothing goes (Fn, say)
  if (!_imeKeys.empty() && _imeKeys.back().id == key_id) {
    _imeKeys.back().raw_no_field = !k.im && k.field && !*k.field;
    _imeKeys.back().im = k.im;
    _imeKeys.back().ack = _imeAck;
  }
  k.seq = _imeSeq;  // not a message of its own: what it holds has its numbers
  [self imeSend:k];
}

// A key-down this Mac sent before it heard of a text field over there, not
// judged yet when the server last said, that the server gives back if a
// field takes input methods there when it comes: one sent as a key, taking
// it that none did, or one of a composition that went nowhere.
- (BOOL)imeAwaitsJudgement {
  for (auto it = _imeKeys.rbegin(); it != _imeKeys.rend() && it->id > _imeJudgedSeen; ++it) {
    if (it->raw_no_field || (it->im && it->ack < _imeTypedSeen)) return YES;
  }
  return NO;
}

// The keys held meanwhile go in, in order.
- (void)imeLetHeldGo {
  _imeHolding = NO;
  std::vector<ImeHeldKey> held;
  held.swap(_imeHeld);
  NSMutableSet<RemoteWindowController*>* late = [NSMutableSet set];
  for (const ImeHeldKey& h : held) {
    RemoteWindowController* w = h.window;
    if (!w || !_session) continue;
    [w key:h.event];
    if (h.releases != w->_releaseGen) [late addObject:w];
  }
  [self imeLetGoAfter:late];
}

// Windows that lost the focus (releaseHeld) after keys that went in only now:
// nothing stays down over there, as when they lost it.
- (void)imeLetGoAfter:(NSSet<RemoteWindowController*>*)late {
  for (RemoteWindowController* w in late) [w releaseHeld];
  if (late.count) _imeTaken.clear();
}

// What the server said about this Mac's keys (6.6.0): it judged some
// otherwise than they were sent, or how far it has judged them.
- (void)imeHeard:(const rdm::ImeState&)s {
  _imeJudgedSeen = std::max(_imeJudgedSeen, s.judged);
  const BOOL typed = s.typed > _imeTypedSeen;
  const BOOL back = s.back > _imeBackSeen;
  if (typed || back) {
    _imeTypedSeen = std::max(_imeTypedSeen, s.typed);
    _imeBackSeen = std::max(_imeBackSeen, s.back);
    _imeAck = std::max(_imeTypedSeen, _imeBackSeen);
    // The composition went nowhere there (its keys were typed as keys), or
    // it is made again below: dropped here without a word, and what this
    // Mac's input method may put in for that is not sent. (Keys typed while
    // one could still come back are held, so a composition is rarely here.)
    if (_imeMarked) {
      _imeDropInsert = YES;
      [[_imeView imeContext] discardMarkedText];
      _imeMarked = nil;
    }
    _imeAhead.reset();
  }
  if (back) {
    // The keys from s.back on, all sent before this was heard, were not
    // used: handled again first, in order, as things stood before the first
    // of them; the keys held meanwhile go after them.
    auto first = std::find_if(_imeKeys.begin(), _imeKeys.end(),
                              [&](const ImeKeyRecord& r) { return r.id >= s.back; });
    if (first != _imeKeys.end()) {
      std::vector<ImeKeyRecord> again(std::make_move_iterator(first), std::make_move_iterator(_imeKeys.end()));
      _imeKeys.erase(first, _imeKeys.end());
      if (RemoteWindowController* w = again.front().window) w->_heldKeys = again.front().held;
      _imeTaken = again.front().taken;
      const BOOL holding = _imeHolding;
      _imeHolding = NO;
      NSMutableSet<RemoteWindowController*>* late = [NSMutableSet set];
      for (const ImeKeyRecord& r : again) {
        RemoteWindowController* w = r.window;
        if (!w || !_session) continue;
        [w key:r.event];  // numbered again (or as before, should the mode have ended)
        if (r.releases != w->_releaseGen) [late addObject:w];
      }
      _imeHolding = holding;
      [self imeLetGoAfter:late];
    }
  }
  // While a key sent before this Mac heard of the text field over there may
  // still come back, the keys typed meanwhile wait: composed now, they would
  // go to the input method before it.
  if ([self imeJudged] && s.client && !s.secure && [self imeAwaitsJudgement]) {
    _imeHolding = YES;
  } else if (_imeHolding || !_imeHeld.empty()) {
    [self imeLetHeldGo];
  }
}

- (void)imeMark:(NSString*)text selected:(NSRange)sel {
  RemoteWindowController* root = [self imeRoot];
  if (root != self) return [root imeMark:text selected:sel];
  if (text.length > 0) _imeDropInsert = NO;  // a new composition: the input method moved on
  if (text.length == 0) {
    if (_imeMarked) [self imeSend:{rdm::ImeInput::Kind::kCancel, ++_imeSeq}];
    _imeMarked = nil;
  } else {
    if (!_imeMarked) _imeComposingSeq = _imeSeq + 1;
    _imeMarked = [text copy];
    rdm::ImeInput m{rdm::ImeInput::Kind::kMark, ++_imeSeq, text.UTF8String ?: ""};
    if (sel.location != NSNotFound && NSMaxRange(sel) <= text.length) {
      m.sel_location = static_cast<uint32_t>(sel.location);
      m.sel_length = static_cast<uint32_t>(sel.length);
    } else {
      m.sel_location = static_cast<uint32_t>(text.length);
    }
    [self imeSend:m];
  }
}

- (void)imeInsert:(NSString*)text {
  RemoteWindowController* root = [self imeRoot];
  if (root != self) return [root imeInsert:text];
  if (_imeDropInsert) return;  // its answer to discardMarkedText (see _imeDropInsert)
  _imeMarked = nil;
  if (text.length) {
    [self imeSend:{rdm::ImeInput::Kind::kCommit, ++_imeSeq, text.UTF8String ?: ""}];
    [self imeGuessAhead:text];
  }
}

// The caret after `text`, for the composition that may start before the
// server's answer (see _imeAhead).
- (void)imeGuessAhead:(NSString*)text {
  if (!_imeState || !_imeState->client) return;
  const std::optional<rdm::ImeRect> from = _imeAhead ? _imeAhead : _imeState->caret;
  if (!from) return;
  if (!_imeAhead) {
    _imeAheadFromX = from->x;
    _imeAheadUnits = 0;
  }
  const double units = [text sizeWithAttributes:@{NSFontAttributeName : [NSFont systemFontOfSize:100]}].width;
  // Before any commit was measured: text about 0.8 of the line height.
  const double per_unit = _imePixelsPerUnit > 0 ? _imePixelsPerUnit : from->height * 0.8 / 100.0;
  rdm::ImeRect r = *from;
  r.x += static_cast<int32_t>(std::lround(units * per_unit));
  _imeAhead = r;
  _imeAheadSeq = _imeSeq;
  _imeAheadUnits += units;
}

// The input method gives the key back (it is not composing, or it is a key
// it has no use for): it goes to the server as a key.
- (void)imeCommand {
  RemoteWindowController* root = [self imeRoot];
  if (root != self) return [root imeCommand];
  if (_imeEvent) _imeEventRaw = YES;
}

// The composition goes in as it stands.
- (void)imeUnmark {
  RemoteWindowController* root = [self imeRoot];
  if (root != self) return [root imeUnmark];
  if (!_imeMarked) return;
  NSString* text = _imeMarked;
  // Dropped over here, put in by us: once, whether or not the input method
  // also answers the discard by putting it in.
  _imeDropInsert = YES;
  [[_imeView imeContext] discardMarkedText];
  _imeDropInsert = NO;
  [self imeInsert:text];
  _imeDropInsert = YES;  // its answer may also come a little later
}

// A click: the composition is dropped on both sides first (the click may
// move the caret or the focus over there).
- (void)imeMouseDown {
  RemoteWindowController* root = [self imeRoot];
  if (root != self) return [root imeMouseDown];
  _imeAhead.reset();  // the caret goes where the click is
  _imeLastClick = NSEvent.mouseLocation;
  _imeClicked = YES;
  if (!_imeMarked) return;
  _imeDropInsert = YES;  // dropped on both sides: nothing of it goes in
  [[_imeView imeContext] discardMarkedText];
  _imeMarked = nil;
  [self imeSend:{rdm::ImeInput::Kind::kCancel, ++_imeSeq}];
}

- (BOOL)imeOffersText {
  RemoteWindowController* root = [self imeRoot];
  return root->_imeMode && root->_imeState && root->_imeState->client && !root->_imeState->secure && !gObserveOnly;
}

// AppKit asks -inputContext again at the end of the event loop's pass; this
// makes sure there is one when the offer changed without an event here (a
// message from the server).
- (void)imeOfferChanged {
  const BOOL offers = [self imeOffersText];
  if (offers == _imeOffered) return;
  _imeOffered = offers;
  [NSApp setWindowsNeedUpdate:YES];
  [NSApp updateWindows];
}

- (BOOL)imeHasMarked {
  return [self imeRoot]->_imeMarked != nil;
}

- (NSRange)imeMarkedRange {
  NSString* m = [self imeRoot]->_imeMarked;
  return m ? NSMakeRange(0, m.length) : NSMakeRange(NSNotFound, 0);
}

// A rectangle in the pixels of this window's picture, in screen points.
- (BOOL)screenRectOfStream:(const rdm::ImeRect&)r out:(NSRect*)out {
  double s, ox, oy;
  if (![self fitScale:&s ox:&ox oy:&oy] || !_window.isVisible) return NO;
  const NSRect inView = NSMakeRect(ox + r.x * s, oy + (_streamH - r.y - r.height) * s, std::max(1.0, r.width * s),
                                   std::max(1.0, r.height * s));
  *out = [_window convertRectToScreen:[_view convertRect:inView toView:nil]];
  return YES;
}

// Where this Mac's candidate window goes: under the remote composition (or
// caret); before the server has said, where it last was, or at the pointer.
- (NSRect)imeCaretRect {
  RemoteWindowController* root = [self imeRoot];
  if (root != self) return [root imeCaretRect];
  if (_imeState && (_imeAhead || _imeState->caret)) {
    const rdm::ImeRect& c = _imeAhead ? *_imeAhead : *_imeState->caret;
    RemoteWindowController* w = nil;
    if (c.display == 0) {
      w = self;
    } else if (c.display > 0 && static_cast<NSUInteger>(c.display) <= _extras.count) {
      w = _extras[static_cast<NSUInteger>(c.display) - 1];
    }
    NSRect r;
    if (w && [w screenRectOfStream:c out:&r]) {
      _imeLastCaret = r;
      return r;
    }
  }
  if (!NSIsEmptyRect(_imeLastCaret) || _imeLastCaret.size.height > 0) return _imeLastCaret;
  // A field that has not said where its caret is yet (Chromium, just
  // focused): it is about where the click that focused it was.
  const NSPoint p = _imeClicked ? _imeLastClick : NSEvent.mouseLocation;
  return NSMakeRect(p.x, p.y - 20, 1, 20);
}

// Transport thread -> main thread.
- (void)imeStateReport:(const rdm::ImeState&)s {
  __weak RemoteWindowController* weakSelf = self;
  const rdm::ImeState copy = s;
  dispatch_async(dispatch_get_main_queue(), ^{
    RemoteWindowController* me = weakSelf;
    if (!me) return;
    if (!me->_session) {
      me->_imeEarly = copy;
      return;
    }
    std::optional<rdm::ImeRect> before = me->_imeState ? me->_imeState->caret : std::nullopt;
    if (me->_imeAhead) {
      // The server's answer to the commits: a caret that already reflects
      // them (seq is what the server's input method had put in when it found
      // that caret, 6.4.0). Their real width teaches the guess.
      const bool answered = copy.caret && copy.seq >= me->_imeAheadSeq;
      if (answered || !copy.client) {
        if (answered && copy.caret->display == me->_imeAhead->display && copy.caret->y == me->_imeAhead->y &&
            me->_imeAheadUnits > 0) {
          const double dx = copy.caret->x - me->_imeAheadFromX;
          const double guess = copy.caret->height * 0.8 / 100.0;
          if (dx > 0 && dx / me->_imeAheadUnits > guess / 3 && dx / me->_imeAheadUnits < guess * 3) {
            me->_imePixelsPerUnit = dx / me->_imeAheadUnits;
          }
        }
        before = me->_imeAhead;  // what the input method was given
        me->_imeAhead.reset();
      }
    }
    // Another text field over there: where the last one's caret was means
    // nothing for it. The same field let go and taken again (macOS does that
    // within a millisecond) keeps it; a server before 6.4.0 does not number
    // fields, then only another app named counts.
    if (copy.field != 0) {
      if (copy.field != me->_imeField) me->_imeLastCaret = NSZeroRect;
      me->_imeField = copy.field;
    } else if (!copy.app.empty()) {
      if (copy.app != me->_imeLastApp) me->_imeLastCaret = NSZeroRect;
      me->_imeLastApp = copy.app;
    }
    me->_imeState = copy;
    if (copy.ended && me->_imeMarked) {
      // Ended over there (a click, another app): this Mac's input method lets
      // go too, and what it may put in for that is not sent.
      me->_imeDropInsert = YES;
      [[me->_imeView imeContext] discardMarkedText];
      me->_imeMarked = nil;
    }
    [me imeUpdateMode];
    [me imeHeard:copy];
    // A new position for the composition under way: the candidate window
    // moves. A caret that reflects everything before the composition's first
    // mark (the commit just before it, say) is where it starts.
    if (me->_imeMarked && copy.caret != before && copy.seq + 1 >= me->_imeComposingSeq) {
      [[me->_imeView imeContext] invalidateCharacterCoordinates];
    }
  });
}


// Main thread. Tells the server which input source this Mac uses, unless it
// already knows (`force`: say it anyway, e.g. when the session starts).
- (void)sendInputSource:(BOOL)force {
  if (!_viewer || !SyncKeyboardLanguage() || gObserveOnly) return;
  // Typing with this Mac's input method: the server stays on RDMachine Input,
  // and its input source changes only when the mode does (6.5.0; switching it
  // at every connection is what lost the text field over there). Only a
  // server that cannot take this Mac's input method follows the keyboard
  // language instead, as does one that says nothing about it.
  if (UseLocalInputMethod() && !(_imeState ? !_imeState->available : _imeServerSilent)) return;
  if (_imeMode) return;  // the server stays on RDMachine Input meanwhile
  rda_input_source_info cur{};
  cur.struct_size = sizeof(cur);
  if (rda_current_input_source(&cur) != RDA_OK) return;
  if (!force && _sentInputSource == cur.id) return;
  const rdm::InputSourceState s{cur.id, cur.name, cur.language, cur.input_method != 0, cur.ascii_capable != 0};
  const std::string payload = rdm::EncodeInputSource(s);
  const auto* bytes = reinterpret_cast<const uint8_t*>(payload.data());
  // On the input channel where the server takes it (6.2 and later): in order
  // with the input method's mode messages, so a switch back to sync is never
  // overtaken by the sync itself (the server ignores sync while it is on
  // RDMachine Input). Older servers: the message channel, as before.
  rda_status st = _imeState ? _session->SendInputMessage(rdm::kInputSourceMessageType, bytes, payload.size())
                            : RDA_ERR_UNSUPPORTED;
  if (st == RDA_ERR_UNSUPPORTED) st = _session->SendMessage(rdm::kInputSourceMessageType, bytes, payload.size());
  if (st == RDA_OK) _sentInputSource = cur.id;
}

- (void)inputSourceChanged:(NSNotification*)note {
  [self imeUpdateMode];
  [self sendInputSource:NO];
}

// Main thread. View > Text Mode changed: the running session follows. (A
// session that starts later asks for it in its session_request.)
- (void)sendPictureMode {
  if (!_viewer) return;
  const std::string payload = rdm::EncodePictureMode({TextMode()});
  _session->SendMessage(rdm::kPictureModeMessageType, reinterpret_cast<const uint8_t*>(payload.data()),
                          payload.size());
  // The desktop's resolution may follow the mode (TextModeStandardResolution).
  [self desktopNeedsUpdate];
}

// Main thread. View > Play Sound changed: the running session's sound is
// paused or resumed (a session that starts later asks for it or not).
- (void)playSoundChanged {
  if (_session) _session->SetAudioPaused(!PlaySound());
}

// Main thread. View > Observe Only changed. Starting to observe: let go of
// what is held (the server would keep it down). Controlling again: the
// server hears this Mac's input source again.
- (void)observeChanged {
  [self imeUpdateMode];
  if (gObserveOnly) {
    [self releaseHeld];
  } else {
    [self sendInputSource:YES];
  }
  [self renderStats];
}

// The overlay's sound line (docs/audio.md, 10): the server's report, what
// session_ready said, and this side's buffer and losses.
- (NSString*)audioLine:(const rdm::SessionStatsReport&)r {
  if (!_session) return @"Audio: –";
  const rdm::SessionReady ready = _session->session();
  if (!ready.audio) {
    return NS(rdm::AudioOffLine(false, ready.audio_reason, _session->revision(), _session->audio_requested(),
                                PlaySound()));
  }
  if (r.audio == "paused" || !PlaySound()) return @"Audio: paused";
  if (r.audio == "silent") return @"Audio: silent";
  if (r.audio == "no_permission") return @"Audio: off (server needs the System Audio Recording permission)";
  if (r.audio == "failed") return @"Audio: off (server could not capture)";
  if (r.audio == "starting" || r.audio.empty()) return @"Audio: starting";
  rda_viewer_stats vs{};
  vs.struct_size = sizeof(vs);
  rda_viewer* v = _session->viewer();
  if (!v || rda_viewer_get_stats(v, &vs) != RDA_OK) vs = rda_viewer_stats{};
  // Concealed frames against frames played (about one per packet).
  const double lost = vs.audio_packets ? 100.0 * static_cast<double>(vs.audio_lost) /
                                             static_cast<double>(vs.audio_packets + vs.audio_lost)
                                       : 0.0;
  return [NSString stringWithFormat:@"Audio: %@ · buffer %d ms · lost %.1f%%", StatsKbps(r.audio_kbps),
                                    vs.audio_buffer_ms, lost];
}

- (void)syncSettingChanged:(NSNotification*)note {
  _sentInputSource.clear();
  [self sendInputSource:YES];
}

- (void)windowDidBecomeKey:(NSNotification*)note {
  // Input methods must work here: nothing of ours may keep secure input on.
  const pid_t holder = ReleaseSecureInput();
  if (holder != _secureInputHolder) {
    _secureInputHolder = holder;
    if (holder != 0) {
      NSLog(@"secure input is on, held by %@: input methods are blocked, so Caps Lock and the input menu cannot "
            @"switch between Chinese and English until it lets go",
            DescribeProcess(holder));
    }
    [self renderStats];
  }
  // It may have been switched while another window was in front.
  [self sendInputSource:NO];
}

- (void)windowDidResignKey:(NSNotification*)note {
  [self releaseHeld];
  [self imeRoot]->_imeTaken.clear();
}

// Transport thread -> main thread.
- (void)statsReport:(const rdm::SessionStatsReport&)r {
  __weak RemoteWindowController* weakSelf = self;
  const rdm::SessionStatsReport copy = r;
  dispatch_async(dispatch_get_main_queue(), ^{
    RemoteWindowController* me = weakSelf;
    if (!me) return;
    me->_stats = copy;
    [me renderStats];
  });
}

// Main thread.
- (void)renderStats {
  if (!_viewer || (!gObserveOnly && (!StatsShown() || !_stats))) {
    _statsLayer.hidden = YES;
    return;
  }
  // Observing: its line shows even with the statistics off, or nothing
  // would say why the keyboard and mouse do nothing.
  const bool shown = StatsShown() && _stats;
  const rdm::SessionStatsReport r = _stats ? *_stats : rdm::SessionStatsReport{};
  NSSet<NSString*>* on = shown ? StatsFieldsOn() : [NSSet set];
  NSMutableArray<NSString*>* lines = [NSMutableArray array];
  if (gObserveOnly) [lines addObject:@"只观看（⌃⌥⌘O 恢复控制）"];
  auto add = [&](NSString* key, NSString* text) {
    if ([on containsObject:key]) [lines addObject:text];
  };
  const long long secs =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - _connectedAt).count();
  add(@"time", [NSString stringWithFormat:@"%02lld:%02lld:%02lld", secs / 3600, secs / 60 % 60, secs % 60]);
  add(@"fps", [NSString stringWithFormat:@"%.0f fps", r.encoded_fps]);
  add(@"latency", r.rtt_ms <= 0   ? @"RTT –"
                  : r.rtt_ms < 10 ? [NSString stringWithFormat:@"RTT %.1f ms", r.rtt_ms]
                                  : [NSString stringWithFormat:@"RTT %.0f ms", r.rtt_ms]);
  add(@"loss", r.recent_loss_pct >= 0 ? [NSString stringWithFormat:@"Loss %.1f%%", r.recent_loss_pct] : @"Loss –");
  add(@"bitrate", StatsKbps(r.bitrate_kbps));
  // Since this window opened, reconnections included; rdaccel's payload
  // (the picture and the sound), without packet headers and resends.
  add(@"traffic", [@"流量 " stringByAppendingString:NS(rdm::FormatTraffic(_trafficBase +
                                                                          (_session ? _session->BytesReceived() : 0)))]);
  add(@"bandwidth", r.bandwidth_kbps > 0 ? [@"BW " stringByAppendingString:StatsKbps(r.bandwidth_kbps)] : @"BW –");
  add(@"resolution", [NSString stringWithFormat:@"%d×%d", r.width, r.height]);
  add(@"encoder", r.encoder.empty() ? @"–" : NS(r.encoder));
  // Not a field to pick: whoever sees soft photos should see why.
  if (shown && r.text_mode) [lines addObject:@"省流量"];
  // Nor this: the input menu is greyed out and nothing here says why.
  _secureInputHolder = SecureInputHolder() == getpid() ? ReleaseSecureInput() : SecureInputHolder();
  if (shown && _secureInputHolder != 0) {
    [lines addObject:[NSString stringWithFormat:@"输入法被 %@ 拦住", DescribeProcess(_secureInputHolder)]];
  }
  add(@"input", r.input_source.empty() ? @"输入法 –" : NS(r.input_source));
  const double response = _responseMs.load();
  add(@"response", response < 0 ? @"操作到画面 –" : [NSString stringWithFormat:@"操作到画面 %.0f ms", response]);
  if ([on containsObject:@"audio"]) [lines addObject:[self audioLine:r]];
  if (lines.count == 0) {
    _statsLayer.hidden = YES;
    return;
  }
  NSDictionary* attrs = @{
    NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightMedium],
    NSForegroundColorAttributeName : NSColor.whiteColor,
  };
  NSAttributedString* text = [[NSAttributedString alloc] initWithString:[lines componentsJoinedByString:@"\n"]
                                                             attributes:attrs];
  const NSSize size = [text boundingRectWithSize:NSMakeSize(400, 400) options:NSStringDrawingUsesLineFragmentOrigin].size;
  const CGFloat padX = 7, padY = 4, margin = 8;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  const CGRect b = _layer.bounds;
  const CGFloat tw = std::ceil(size.width), th = std::ceil(size.height);
  const CGFloat w = tw + 2 * padX, h = th + 2 * padY;
  _statsLayer.frame = CGRectMake(b.size.width - w - margin, b.size.height - h - margin, w, h);
  _statsLayer.backgroundColor = [NSColor colorWithWhite:0 alpha:StatsOpacity() / 100.0].CGColor;
  _statsText.frame = CGRectMake(padX, padY, tw, th);
  _statsText.contentsScale = _window.backingScaleFactor;
  _statsText.string = text;
  _statsLayer.hidden = NO;
  [CATransaction commit];
}

- (void)windowDidResize:(NSNotification*)note {
  [self desktopNeedsUpdate];  // zoom, tiling, a script: resizes without a drag
  [self renderStats];
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
  [self updatePictureFilter];
  if (!first || w <= 0 || h <= 0 || _extraIndex >= 0) return;
  // Full screen, or on its way there: the screen decides the size.
  if (_enteringFullScreen || (_window.styleMask & NSWindowStyleMaskFullScreen)) return;
  const NSRect screen = (_window.screen ?: NSScreen.mainScreen).visibleFrame;
  // One stream pixel per backing pixel; per point for a 1x desktop asked
  // for on a Retina screen (text mode).
  const double scale = DesktopScale(_window.backingScaleFactor);
  double cw = w / scale, ch = h / scale;
  const double fit = std::min({1.0, screen.size.width / cw, screen.size.height / ch});
  [_window setContentSize:NSMakeSize(cw * fit, ch * fit)];
  // The server's own display keeps its proportions; a desktop built for
  // this window takes whatever shape the window is given.
  if (!_desktopBuilt) _window.contentAspectRatio = NSMakeSize(w, h);
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

- (BOOL)streamPoint:(NSPoint)p x:(int32_t*)x y:(int32_t*)y clamp:(BOOL)clamp {
  double s, ox, oy;
  if (![self fitScale:&s ox:&ox oy:&oy]) return NO;
  double sx = (p.x - ox) / s, sy = _streamH - (p.y - oy) / s;  // view origin is bottom left
  if (sx < 0 || sy < 0 || sx >= _streamW || sy >= _streamH) {
    if (!clamp) return NO;
    sx = std::clamp(sx, 0.0, static_cast<double>(_streamW - 1));
    sy = std::clamp(sy, 0.0, static_cast<double>(_streamH - 1));
  }
  *x = static_cast<int32_t>(sx);
  *y = static_cast<int32_t>(sy);
  return YES;
}

- (BOOL)buttonHeld {
  return !_heldButtons.empty();
}

- (void)send:(const rda_input_event&)ev {
  if (gObserveOnly && !_releasing) return;  // observing: the server gets no input
  if (ev.kind == RDA_INPUT_KEY_DOWN || ev.kind == RDA_INPUT_MOUSE_DOWN || ev.kind == RDA_INPUT_SCROLL) {
    // Only on a still screen (no picture for 150 ms): otherwise the next
    // picture may be one that was on its way anyway.
    const uint64_t now = rda_now_us();
    if (_inputPendingUs.load() == 0 && now - _lastFrameUs.load() > 150'000) _inputPendingUs = now;
  }
  if (_extraIndex >= 0 ? !_extraLive : !_viewer) return;
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
  if (_extraIndex >= 0 && (ev.kind == RDA_INPUT_KEY_DOWN || ev.kind == RDA_INPUT_KEY_UP) && _parent) {
    // Keys go through the main picture's connection whichever window has
    // them (they go to the focused app, not to a display): there they stay
    // in order with the input method's text (docs/ime.md).
    [_parent send:ev];
  } else if (_extraIndex >= 0) {
    [_parent sendExtra:static_cast<NSUInteger>(_extraIndex) event:ev];
  } else if (_imeCapture && (ev.kind == RDA_INPUT_KEY_DOWN || ev.kind == RDA_INPUT_KEY_UP)) {
    _imeCaptured.push_back(ev);  // goes over with the key's number (imeKey:from:)
  } else {
    _session->SendInput(ev);
  }
}

- (void)key:(NSEvent*)e {
  RemoteWindowController* root = [self imeRoot];
  if ([root imeJudged]) return [root imeKey:e from:self];
  if ([root imeTakes:e from:self]) return;
  [self rawKey:e];
}

// The key as rawKey sends a key-down or key-up (NO: no HID code, e.g. Fn).
- (BOOL)keyEventOf:(NSEvent*)e into:(rda_input_event*)out {
  rda_input_event ev = Event(e.type == NSEventTypeKeyDown ? RDA_INPUT_KEY_DOWN : RDA_INPUT_KEY_UP);
  ev.modifiers = Modifiers(e.modifierFlags);
  ev.code = rda_keycode_from_mac(e.keyCode);
  if (ev.code == 0) return NO;
  *out = ev;
  return YES;
}

// A key as a key: the server's keyboard layout (and input method, without
// the client's) makes it a character.
- (void)rawKey:(NSEvent*)e {
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
  ++_releaseGen;
  const std::set<uint32_t> keys = _heldKeys, buttons = _heldButtons;
  _releasing = YES;
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
  _releasing = NO;
}

// Decode thread. Copies what changed into the surface that is not on screen
// and hands it to the main thread; while the main thread is still busy with
// the previous one, only remembers what changed.
- (void)frame:(const rda_image*)img changed:(const rda_rect*)changed count:(size_t)count {
  {
    const uint64_t now = rda_now_us();
    const uint64_t pending = _inputPendingUs.exchange(0);
    if (pending != 0 && now > pending) _responseMs = static_cast<double>(now - pending) / 1000.0;
    _lastFrameUs = now;
  }
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
