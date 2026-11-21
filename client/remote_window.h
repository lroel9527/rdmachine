// The remote screen's window: the picture, the remote cursor, local input, the
// statistics overlay, full screen and the extra displays of an extended desktop.
#pragma once
#include "client_support.h"

@class RemoteWindowController;

// The picture, the remote cursor on top, and all local input. It is the text
// input client of this Mac's input method when the client types with it
// (docs/ime.md): what the input method composes and commits goes to the
// server, and its candidate window is put next to the remote caret.
@interface RemoteView : NSView <NSTextInputClient>
@property(nonatomic, weak) RemoteWindowController* owner;
// Its text input context whether or not it offers one to macOS now (see
// -inputContext): for the input method calls the session makes itself.
- (NSTextInputContext*)imeContext;
@property(nonatomic) BOOL pointerInside;
// Mouse moves count even when the window is not the key one: the windows of
// an extended desktop, where the pointer goes from screen to screen.
@property(nonatomic) BOOL trackAlways;
@end

// A borderless window that can still take the keyboard (an extra display's).
@interface ExtraDisplayWindow : NSWindow
@end

// One session's window. Viewer callbacks (library threads) reach it through
// the C functions below; it is created before the session starts so pictures
// arriving during the handshake have somewhere to go.
@interface RemoteWindowController : NSObject <NSWindowDelegate>
@property(nonatomic, readonly) NSWindow* window;
@property(nonatomic, copy) void (^onClose)(void);
// The server this window shows: the address as typed, and the name it gave.
@property(nonatomic, copy) NSString* target;
@property(nonatomic, copy) NSString* serverName;
// Set while it is connecting again after a drop (for the list), and until
// when it keeps trying.
@property(nonatomic, strong) NSDate* reconnectingSince;
@property(nonatomic, strong) NSDate* reconnectUntil;
// The window was closed: nothing connects into it any more.
@property(nonatomic, readonly) BOOL closed;
// A session is attached (the picture is live).
- (BOOL)isLive;
// "已连接 12 分钟 · UDP · 18 ms · 2880×1800", for the list.
- (NSString*)liveDetail;
- (instancetype)initWithTitle:(NSString*)title;
// The desktop to ask the server for: this window's picture area at its
// largest comfortable size on its screen, in that screen's pixels.
- (rdm::DesktopSpec)initialDesktop;
- (rdm::DesktopSpec)viewDesktop;
- (void)saveWindowMode;
- (void)enterFullScreen;
// `activate`: bring the window to the front (a new connection); a reconnect
// leaves the windows as the user has them.
- (void)attachSession:(std::unique_ptr<rdm::RemoteSession>)session activate:(BOOL)activate;
- (void)sessionMoved:(const rdm::SessionReady&)ready;
// A message over the picture (interrupted, switching, reconnecting) with a
// Disconnect button; hidden again when the picture is back.
- (void)showStatus:(NSString*)title
            detail:(NSString*)detail
            symbol:(NSString*)symbol
             until:(NSDate*)deadline
          canRetry:(BOOL)canRetry;
- (void)hideStatus;
// "立即重试" on the status card (a reconnect waiting for its next try).
@property(nonatomic, copy) void (^onRetry)(void);
// Extended desktop: whether this session asked for extra displays, and what
// to do when that should change (full screen entered or left, the setting
// changed): the app connects again with the new layout.
@property(nonatomic) BOOL extrasRequested;
@property(nonatomic, copy) void (^onDisplaysChanged)(void);
- (BOOL)wantsFullScreen;
- (NSScreen*)homeScreen;
// An extra display's window, full size on `screen`, its picture and input
// going through `parent`'s session.
- (instancetype)initExtraFor:(RemoteWindowController*)parent index:(NSUInteger)index screen:(NSScreen*)screen;
- (void)sendExtra:(NSUInteger)index event:(const rda_input_event&)ev;
- (void)extraConnected;
- (void)toggleFullScreenFromExtra;
- (void)endSession;
- (void)send:(const rda_input_event&)ev;
// The picture's pixel under a point of the view; NO outside the picture,
// unless `clamp` (then the nearest pixel of it).
- (BOOL)streamPoint:(NSPoint)p x:(int32_t*)x y:(int32_t*)y clamp:(BOOL)clamp;
- (BOOL)buttonHeld;
- (NSCursor*)remoteCursor;
- (void)placeCursor;
- (void)key:(NSEvent*)e;
- (BOOL)localShortcut:(NSEvent*)e;
// Library threads.
- (void)frame:(const rda_image*)img changed:(const rda_rect*)changed count:(size_t)count;
- (void)streamInfo:(int32_t)w height:(int32_t)h;
- (void)cursor:(const rda_cursor_state*)c;
- (void)statsReport:(const rdm::SessionStatsReport&)r;
// The client's input method (docs/ime.md). imeStateReport: on the transport
// thread; the rest on the main thread, from RemoteView's text input client.
- (void)imeStateReport:(const rdm::ImeState&)s;
- (void)imeMark:(NSString*)text selected:(NSRange)sel;
- (void)imeInsert:(NSString*)text;
- (void)imeCommand;
- (void)imeUnmark;
- (void)imeMouseDown;
- (BOOL)imeHasMarked;
- (NSRange)imeMarkedRange;
- (NSRect)imeCaretRect;
// Whether a text field that takes input methods has the focus over there (and
// this Mac's input method is used): only then is the remote view a text field
// for macOS, with an input method and its input source indicator (6.5.4).
- (BOOL)imeOffersText;
@end

namespace rdmclient {

// The viewer's callbacks (rda_viewer_config) for a RemoteWindowController
// passed as `user`. They run on the library's threads.
void OnStreamInfo(void* user, int32_t w, int32_t h);
void OnFrame(void* user, const rda_image* img, const rda_rect* changed, size_t n, const rda_frame_info*);
void OnCursor(void* user, const rda_cursor_state* c);
void OnMessage(void* user, uint32_t type, const uint8_t* payload, size_t n);

}  // namespace rdmclient
