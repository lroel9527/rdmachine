// The server's menu bar icon, settings window and floating statistics panel.
// Everything here runs on the main thread; ServerRuntime is thread-safe.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>

#include <dlfcn.h>
#include <signal.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "mac_app.h"
#include "rdmachine/session/client_input_icon.h"
#include "rdmachine/session/file_progress.h"

namespace {

NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

NSString* HMS(std::chrono::seconds s) {
  const long long t = s.count();
  return [NSString stringWithFormat:@"%02lld:%02lld:%02lld", t / 3600, t / 60 % 60, t % 60];
}

NSString* PeerText(const rdm::MediaHostStatus& st) {
  return [NSString stringWithFormat:@"%@@%@", NS(st.origin.username), NS(st.origin.peer_ip)];
}

// Keyboard input source sync, for the panel and the menu.
NSString* InputSourceText(const rdm::MediaHostStatus& st) {
  if (st.client_input_source.empty()) return @"未同步（客户端没有发来）";
  if (st.input_source.empty()) return [NSString stringWithFormat:@"本机没有对应「%@」的输入法", NS(st.client_input_source)];
  if (st.input_source == st.client_input_source) return NS(st.input_source);
  return [NSString stringWithFormat:@"%@（客户端：%@）", NS(st.input_source), NS(st.client_input_source)];
}

// The System Audio Recording permission (TCC kTCCServiceAudioCapture): no
// public call asks or tells, so TCC's own (private, looked up at run time;
// docs/audio.md, D5). 0 granted, 1 denied, 2 not asked yet, -1 not known.
// TCC answers each process from a cache: a choice made while the server
// runs may show only after it restarts.
void* TccHandle() {
  static void* h = dlopen("/System/Library/PrivateFrameworks/TCC.framework/TCC", RTLD_LAZY);
  return h;
}

int AudioCapturePermission() {
  using Fn = int (*)(CFStringRef, CFDictionaryRef);
  static Fn preflight = TccHandle() ? reinterpret_cast<Fn>(dlsym(TccHandle(), "TCCAccessPreflight")) : nullptr;
  if (!preflight) return -1;
  const int r = preflight(CFSTR("kTCCServiceAudioCapture"), nullptr);
  return r >= 0 && r <= 2 ? r : -1;
}

// Asks the user (once; the answer is remembered by macOS).
void RequestAudioCapturePermission() {
  using Fn = void (*)(CFStringRef, CFDictionaryRef, void (^)(Boolean));
  static Fn request = TccHandle() ? reinterpret_cast<Fn>(dlsym(TccHandle(), "TCCAccessRequest")) : nullptr;
  if (request) request(CFSTR("kTCCServiceAudioCapture"), nullptr, ^(Boolean) {});
}

// The sound's line in the menu (docs/audio.md, 10).
NSString* AudioText(const rdm::MediaHostStatus& st) {
  if (!st.audio) {
    if (st.audio_reason == "disabled") return @"声音：未传（设置里关了）";
    if (st.audio_reason == "login_window") return @"声音：未传（登录窗口）";
    if (st.audio_reason == "unsupported") return @"声音：未传（这台 Mac 采集不了）";
    return @"声音：未传（客户端没要）";
  }
  const rda_audio_status& a = st.audio_status;
  switch (a.state) {
    case RDA_AUDIO_STATE_RUNNING:
      // Without the permission the capture hears only silence, and TCC may
      // not tell this process of a change it made while running.
      if (a.silent && AudioCapturePermission() != 0) return @"声音：安静（也可能是没有系统声音录制权限）";
      if (a.silent) return @"声音：安静";
      return a.muted_local ? @"声音：正在传（本机不出声）" : @"声音：正在传";
    case RDA_AUDIO_STATE_PAUSED: return @"声音：暂停（客户端听不到）";
    case RDA_AUDIO_STATE_NO_PERMISSION: return @"声音：未传（没有系统声音录制权限）";
    case RDA_AUDIO_STATE_FAILED: return [NSString stringWithFormat:@"声音：未传（%@）", NS(a.message)];
    default: return @"声音：正在开始";
  }
}

void Copy(NSString* text) {
  NSPasteboard* pb = [NSPasteboard generalPasteboard];
  [pb clearContents];
  [pb setString:text forType:NSPasteboardTypeString];
}

const rdm::ClipboardMode kClipboardModes[] = {rdm::ClipboardMode::kBoth, rdm::ClipboardMode::kSend,
                                              rdm::ClipboardMode::kReceive, rdm::ClipboardMode::kOff};
NSString* const kClipboardTitles[] = {@"双向", @"只从本机发出", @"只接收", @"关闭"};

void StopApp() {
  [NSApp stop:nil];
  // stop: takes effect after the next event; post one so run returns now.
  NSEvent* e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                  location:NSZeroPoint
                             modifierFlags:0
                                 timestamp:0
                              windowNumber:0
                                   context:nil
                                   subtype:0
                                     data1:0
                                     data2:0];
  [NSApp postEvent:e atStart:YES];
}

}  // namespace

// ---- settings window ---------------------------------------------------------

@interface RDMSettingsWindow : NSObject <NSWindowDelegate>
@property(nonatomic, copy) void (^onSave)(const rdm::ServerSettings&);
- (void)showWithSettings:(const rdm::ServerSettings&)s overrides:(NSString*)overrides;
@end

@implementation RDMSettingsWindow {
  NSWindow* _window;
  NSTextField* _port;
  NSButton* _viewOnly;
  NSPopUpButton* _clipboard;
  NSTextField* _fps;
  NSTextField* _bitrate;
  NSButton* _sendAudio;
  NSButton* _muteLocal;
  NSTextField* _note;
}

static NSTextField* NumberField(int lo, int hi) {
  NSTextField* f = [NSTextField textFieldWithString:@""];
  NSNumberFormatter* nf = [[NSNumberFormatter alloc] init];
  nf.allowsFloats = NO;
  nf.minimum = @(lo);
  nf.maximum = @(hi);
  nf.usesGroupingSeparator = NO;
  f.formatter = nf;
  [f.widthAnchor constraintEqualToConstant:80].active = YES;
  return f;
}

static NSView* WithHint(NSView* field, NSString* hint) {
  NSTextField* h = [NSTextField labelWithString:hint];
  h.textColor = NSColor.secondaryLabelColor;
  h.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
  NSStackView* s = [NSStackView stackViewWithViews:@[ field, h ]];
  s.spacing = 8;
  return s;
}

- (void)build {
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 440, 300)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                          backing:NSBackingStoreBuffered
                                            defer:YES];
  _window.title = @"RDMachine Server 设置";
  _window.releasedWhenClosed = NO;
  _window.delegate = self;

  _port = NumberField(1, 65534);
  _viewOnly = [NSButton checkboxWithTitle:@"只看不控（不接受对方的键盘和鼠标）" target:nil action:nil];
  _clipboard = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  for (NSString* t : kClipboardTitles) [_clipboard addItemWithTitle:t];
  _fps = NumberField(0, 240);
  _bitrate = NumberField(0, 1000000);
  _sendAudio = [NSButton checkboxWithTitle:@"把这台 Mac 的声音传给客户端" target:self action:@selector(audioChanged:)];
  _muteLocal = [NSButton checkboxWithTitle:@"传声音时这台 Mac 自己不出声" target:nil action:nil];
  NSStackView* audio = [NSStackView stackViewWithViews:@[ _sendAudio, _muteLocal ]];
  audio.orientation = NSUserInterfaceLayoutOrientationVertical;
  audio.alignment = NSLayoutAttributeLeading;
  audio.spacing = 6;
  _note = [NSTextField wrappingLabelWithString:@""];
  _note.textColor = NSColor.systemOrangeColor;
  _note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];

  NSGridView* grid = [NSGridView gridViewWithViews:@[
    @[ [NSTextField labelWithString:@"端口："], WithHint(_port, @"TCP 控制 + UDP 画面") ],
    @[ [NSTextField labelWithString:@"控制："], _viewOnly ],
    @[ [NSTextField labelWithString:@"剪贴板："], _clipboard ],
    @[ [NSTextField labelWithString:@"帧率上限："], WithHint(_fps, @"fps，0 = 默认（60）") ],
    @[ [NSTextField labelWithString:@"码率上限："], WithHint(_bitrate, @"kbps，0 = 默认（8000）") ],
    @[ [NSTextField labelWithString:@"声音："], audio ],
  ]];
  grid.rowSpacing = 10;
  grid.columnSpacing = 8;
  [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
  grid.rowAlignment = NSGridRowAlignmentFirstBaseline;

  NSButton* cancel = [NSButton buttonWithTitle:@"取消" target:self action:@selector(cancel:)];
  cancel.keyEquivalent = @"\e";
  NSButton* save = [NSButton buttonWithTitle:@"保存" target:self action:@selector(save:)];
  save.keyEquivalent = @"\r";
  NSStackView* buttons = [NSStackView stackViewWithViews:@[ cancel, save ]];
  NSStackView* root = [NSStackView stackViewWithViews:@[ grid, _note, buttons ]];
  root.orientation = NSUserInterfaceLayoutOrientationVertical;
  root.alignment = NSLayoutAttributeTrailing;
  root.spacing = 16;
  root.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
  [_note.widthAnchor constraintEqualToAnchor:grid.widthAnchor].active = YES;
  _window.contentView = root;
}

- (void)showWithSettings:(const rdm::ServerSettings&)s overrides:(NSString*)overrides {
  if (!_window) [self build];
  _port.integerValue = s.port;
  _viewOnly.state = s.view_only ? NSControlStateValueOn : NSControlStateValueOff;
  for (size_t i = 0; i < 4; ++i) {
    if (kClipboardModes[i] == s.clipboard_mode) [_clipboard selectItemAtIndex:static_cast<NSInteger>(i)];
  }
  _fps.integerValue = s.fps;
  _bitrate.integerValue = s.bitrate_kbps;
  _sendAudio.state = s.send_audio ? NSControlStateValueOn : NSControlStateValueOff;
  _muteLocal.state = s.mute_local_audio ? NSControlStateValueOn : NSControlStateValueOff;
  _muteLocal.enabled = s.send_audio;
  _note.stringValue = overrides.length
                          ? [NSString stringWithFormat:@"这次启动用了命令行参数 %@，对应的设置在退出前以命令行为准。", overrides]
                          : @"保存后服务会用新设置重新开始监听，正在进行的连接会断开。";
  [_window center];
  [NSApp activateIgnoringOtherApps:YES];
  [_window makeKeyAndOrderFront:nil];
}

- (void)cancel:(id)sender {
  [_window close];
}

- (void)audioChanged:(id)sender {
  _muteLocal.enabled = _sendAudio.state == NSControlStateValueOn;
}

- (void)save:(id)sender {
  [_window makeFirstResponder:nil];  // commit the field being edited
  rdm::ServerSettings s;
  s.port = static_cast<uint16_t>(std::clamp<NSInteger>(_port.integerValue, 1, 65534));
  s.view_only = _viewOnly.state == NSControlStateValueOn;
  s.clipboard_mode = kClipboardModes[std::clamp<NSInteger>(_clipboard.indexOfSelectedItem, 0, 3)];
  s.fps = static_cast<int32_t>(std::clamp<NSInteger>(_fps.integerValue, 0, 240));
  s.bitrate_kbps = static_cast<int32_t>(std::clamp<NSInteger>(_bitrate.integerValue, 0, 1000000));
  s.send_audio = _sendAudio.state == NSControlStateValueOn;
  s.mute_local_audio = _muteLocal.state == NSControlStateValueOn;
  if (self.onSave) self.onSave(s);
  [_window close];
}

@end

// ---- menu bar ----------------------------------------------------------------

// The menu bar icon with the input source the client types with in its lower
// left corner (docs/ime.md, 7.1.0), standing out a little past the picture
// and apart from it, so the symbol's person stays whole on the right. A
// template, as the symbol: the client sends its icon as one.
static NSImage* RDMWithInputSource(NSImage* symbol, NSImage* badge) {
  const NSSize s = symbol.size;
  const CGFloat side = std::round(s.height * 0.62);
  const CGFloat out = std::round(side * 0.4);
  const NSSize size = NSMakeSize(s.width + out, s.height);
  NSImage* img = [NSImage imageWithSize:size
                                flipped:NO
                         drawingHandler:^BOOL(NSRect) {
                           [symbol drawInRect:NSMakeRect(out, 0, s.width, s.height)];
                           const NSRect r = NSMakeRect(0, 0, side, side);
                           NSGraphicsContext* g = NSGraphicsContext.currentContext;
                           g.compositingOperation = NSCompositingOperationClear;
                           [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, -1.5, -1.5)
                                                            xRadius:side * 0.3
                                                            yRadius:side * 0.3] fill];
                           g.compositingOperation = NSCompositingOperationSourceOver;
                           [badge drawInRect:r
                                    fromRect:NSZeroRect
                                   operation:NSCompositingOperationSourceOver
                                    fraction:1];
                           return YES;
                         }];
  [img setTemplate:YES];  // not img.template: a C++ keyword in Objective-C++
  return img;
}

@interface RDMMenuBarController : NSObject <NSMenuDelegate>
- (instancetype)initWithRuntime:(rdm::ServerRuntime*)runtime info:(const rdm::MenuBarAppInfo&)info;
@end

@implementation RDMMenuBarController {
  rdm::ServerRuntime* _runtime;
  rdm::MenuBarAppInfo _info;
  NSStatusItem* _item;
  NSMenu* _menu;
  RDMSettingsWindow* _settings;
  NSTimer* _timer;
  bool _menuOpen;
  NSString* _symbol;  // the icon now shown
  std::string _inputIcon;  // ... and the client's input source on it (fingerprint; empty: none)
  int _ringStep;      // the receive ring now shown (-1: none; 0..40, or 41 when the size is unknown)
}

- (instancetype)initWithRuntime:(rdm::ServerRuntime*)runtime info:(const rdm::MenuBarAppInfo&)info {
  if (!(self = [super init])) return nil;
  _runtime = runtime;
  _info = info;
  _menuOpen = false;
  _ringStep = -1;
  _item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
  _item.button.toolTip = @"RDMachine Server";
  _menu = [[NSMenu alloc] init];
  _menu.delegate = self;
  _menu.autoenablesItems = NO;
  _item.menu = _menu;

  __weak RDMMenuBarController* weakSelf = self;
  _settings = [[RDMSettingsWindow alloc] init];
  _settings.onSave = ^(const rdm::ServerSettings& s) {
    [weakSelf applySettings:s];
  };

  // Files pasted here on their way: the icon becomes a ring (Finder shows
  // nothing until the download is over).
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(receiveChanged:)
                                             name:@(rdm::kFileReceiveChangedNotification)
                                           object:nil];
  // The input source the client types with: on the icon as soon as it changes.
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(clientInputIconChanged:)
                                             name:@(rdm::kClientInputIconChangedNotification)
                                           object:nil];
  [self tick];
  // Common modes: keep updating while the menu is open.
  _timer = [NSTimer timerWithTimeInterval:1.0 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
  return self;
}

- (void)clientInputIconChanged:(NSNotification*)note {
  [self tick];
}

- (void)receiveChanged:(NSNotification*)note {
  const rdm::FileReceiveProgress p = rdm::CurrentFileReceive();
  int step = -1;
  if (p.active) step = p.total > 0 ? static_cast<int>(40.0 * std::min<uint64_t>(p.bytes, p.total) / p.total) : 41;
  if (step == _ringStep) return;  // only when it changes: a new image relays out the item
  _ringStep = step;
  if (step < 0) {
    _symbol = nil;  // the usual icon again, at the next tick
    _item.button.toolTip = @"RDMachine Server";
    [self tick];
    return;
  }
  _item.button.image = RDMFileProgressRing(step > 40 ? -1 : step / 40.0);
  _item.button.title = @"";
  _item.button.toolTip = @(rdm::DescribeFileReceive(p).c_str());
}

- (void)tick {
  const rdm::MediaHostStatus st = _runtime->Status();
  if (_ringStep >= 0) {
    // While a receive is shown: its line in the tooltip and the menu.
    _item.button.toolTip = @(rdm::DescribeFileReceive(rdm::CurrentFileReceive()).c_str());
    if (!_menuOpen) [self rebuildMenu];
    return;
  }

  // Only when it changes: setting the image relays out the status item,
  // which can swallow a click that is in progress.
  NSString* symbol = st.running ? @"rectangle.inset.filled.and.person.filled" : @"display";
  const rdm::ClientInputIcon input = st.running ? rdm::CurrentClientInputIcon() : rdm::ClientInputIcon{};
  if (![symbol isEqualToString:_symbol] || input.fingerprint != _inputIcon) {
    if (input.fingerprint != _inputIcon) {
      NSLog(@"menu bar icon: the client's input source %s",
            input.fingerprint.empty() ? "no longer shown" : input.fingerprint.c_str());
    }
    _symbol = symbol;
    _inputIcon = input.fingerprint;
    NSImage* img = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:@"RDMachine Server"];
    [img setTemplate:YES];  // not img.template: a C++ keyword in Objective-C++
    if (img && !input.png.empty()) {
      NSImage* badge = [[NSImage alloc] initWithData:[NSData dataWithBytes:input.png.data() length:input.png.size()]];
      if (badge) img = RDMWithInputSource(img, badge);
    }
    _item.button.image = img;
    _item.button.title = img ? @"" : (st.running ? @"RD●" : @"RD");
  }
  // Keep the menu filled at all times: macOS 26 and later host status items
  // out of process and do not open one whose menu is empty, so filling it
  // only in menuNeedsUpdate: left a menu that never opened.
  if (!_menuOpen) [self rebuildMenu];
}

- (NSMenuItem*)add:(NSString*)title action:(SEL)action {
  NSMenuItem* item = [_menu addItemWithTitle:title action:action keyEquivalent:@""];
  item.target = self;
  item.enabled = action != nullptr;
  return item;
}

- (void)menuWillOpen:(NSMenu*)menu {
  _menuOpen = true;
}

- (void)menuDidClose:(NSMenu*)menu {
  _menuOpen = false;
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
  [self rebuildMenu];
}

- (void)rebuildMenu {
  NSMenu* menu = _menu;
  [menu removeAllItems];
  const rdm::MediaHostStatus st = _runtime->Status();
  const uint16_t port = _runtime->port();

  [self add:[NSString stringWithFormat:@"RDMachine Server %@", NS(_info.version)] action:nullptr];
  const rdm::FileReceiveProgress receiving = rdm::CurrentFileReceive();
  if (receiving.active) [self add:@(rdm::DescribeFileReceive(receiving).c_str()) action:nullptr];
  if (_runtime->agent_mode()) {
    // The daemon listens; this app shows this session's desktop for it.
    [self add:(_runtime->daemon_connected()
                   ? [NSString stringWithFormat:@"服务运行中：端口 %u（守护进程，登录账号：%@）", port, NS(_info.user)]
                   : @"正在等待守护进程（sudo rdmachine-service start）")
        action:nullptr];
  } else {
    [self add:(port ? [NSString stringWithFormat:@"正在监听端口 %u（登录账号：%@）", port, NS(_info.user)]
                    : @"服务未运行")
        action:nullptr];
  }
  if (st.running) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - st.started);
    [self add:[NSString stringWithFormat:@"已连接：%@ · %@", PeerText(st), HMS(secs)]
        action:nullptr];
    [self add:[NSString stringWithFormat:@"键盘语言：%@", InputSourceText(st)] action:nullptr];
    [self add:AudioText(st) action:nullptr];
    if (st.input_unavailable) {
      [self add:@"这次连接只能看不能控：没有辅助功能权限" action:@selector(openAccessibility:)];
    }
    [self add:@"断开当前连接" action:@selector(disconnect:)];
  } else {
    [self add:@"无连接" action:nullptr];
  }

  [menu addItem:NSMenuItem.separatorItem];
  NSMenuItem* fp = [self add:@"复制本机指纹" action:@selector(copyFingerprint:)];
  fp.toolTip = [self fingerprint];

  [menu addItem:NSMenuItem.separatorItem];
  const bool capture = CGPreflightScreenCaptureAccess();
  // The same check rdaccel makes before it injects input.
  const bool input = CGPreflightPostEventAccess();
  [self add:(capture ? @"屏幕录制：已允许" : @"屏幕录制：未允许（点此打开系统设置）")
      action:(capture ? nullptr : @selector(openScreenRecording:))];
  [self add:(input ? @"辅助功能：已允许" : @"辅助功能：未允许，只能看不能控（点此打开系统设置）")
      action:(input ? nullptr : @selector(openAccessibility:))];
  const int sound = AudioCapturePermission();
  [self add:(sound == 0 ? @"系统声音录制：已允许" : @"系统声音录制：未允许（点此打开系统设置）")
      action:(sound == 0 ? nullptr : @selector(openAudioCapture:))];
  if (sound != 0) {
    // macOS answers this question once per process: a change in System
    // Settings shows here (and, for a refusal, in the session) only after
    // a restart; a grant starts the sound anyway once something plays.
    [self add:@"    在系统设置里改了之后，退出 RDMachine Server 再打开，这里才显示新的状态" action:nullptr];
  }
  if (!capture || !input || sound != 0) {
    [self add:@"    在系统设置里打开 RDMachine Server 的开关；下次连接还不行就退出重开" action:nullptr];
  }
  // Typing with the client's input method (docs/ime.md, 7.5): RDMachine
  // Input has to be added once by hand; macOS does not let a program do it.
  if (st.ime == "not enabled") {
    [self add:@"客户端输入法：未启用（点此打开键盘设置）" action:@selector(openKeyboardSettings:)];
    [self add:@"    输入法 → 编辑… → 左下角「+」→ 英语 → RDMachine Input → 添加" action:nullptr];
  } else if (!st.ime.empty()) {
    [self add:(st.ime == "in use" ? @"客户端输入法：正在用" : @"客户端输入法：已启用") action:nullptr];
  }

  [menu addItem:NSMenuItem.separatorItem];
  NSMenuItem* settings = [self add:@"设置…" action:@selector(showSettings:)];
  settings.keyEquivalent = @",";
  [menu addItem:NSMenuItem.separatorItem];
  NSMenuItem* quit = [self add:@"退出 RDMachine Server" action:@selector(quit:)];
  quit.keyEquivalent = @"q";
}

- (void)disconnect:(id)sender {
  _runtime->Disconnect();
}

// As an agent the fingerprint is the daemon's, known once the link to it is
// up, which may be after this app started (and again after the daemon made
// a new identity).
- (NSString*)fingerprint {
  if (_runtime->agent_mode()) {
    const std::string fp = _runtime->daemon_fingerprint();
    if (!fp.empty()) return NS(fp);
  }
  return NS(_info.fingerprint);
}

- (void)copyFingerprint:(id)sender {
  Copy([self fingerprint]);
}

- (void)openScreenRecording:(id)sender {
  CGRequestScreenCaptureAccess();  // adds the app to the list (asks once)
  [NSWorkspace.sharedWorkspace
      openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"]];
}

- (void)openAudioCapture:(id)sender {
  if (AudioCapturePermission() == 2) RequestAudioCapturePermission();  // asks once
  [NSWorkspace.sharedWorkspace
      openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AudioCapture"]];
}

- (void)openAccessibility:(id)sender {
  CGRequestPostEventAccess();  // adds the app to the list (asks once)
  [NSWorkspace.sharedWorkspace
      openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"]];
}

- (void)openKeyboardSettings:(id)sender {
  [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.Keyboard-Settings.extension"]];
}

- (void)showSettings:(id)sender {
  [_settings showWithSettings:_runtime->saved_settings() overrides:NS(_runtime->overrides().Describe())];
}

- (void)applySettings:(const rdm::ServerSettings&)s {
  if (s == _runtime->saved_settings()) return;  // nothing changed: keep the connection
  if (_runtime->Status().running) {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"保存设置会断开当前连接";
    a.informativeText = @"服务要用新设置重新开始监听。";
    [a addButtonWithTitle:@"保存并断开"];
    [a addButtonWithTitle:@"取消"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;
  }
  std::string error;
  if (!_runtime->ApplySettings(s, &error)) {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"新设置没有生效";
    a.informativeText = [NSString stringWithFormat:@"%@\n服务继续用原来的设置运行。", NS(error)];
    [a runModal];
  }
  [self tick];
}

- (void)quit:(id)sender {
  StopApp();
}

@end

namespace rdm {

void RunMenuBarApp(ServerRuntime& runtime, const MenuBarAppInfo& info) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    // A menu bar app: no Dock icon or main menu (LSUIElement in the bundle;
    // set here too for the bare binary).
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    std::vector<dispatch_source_t> sources;
    for (int sig : {SIGINT, SIGTERM}) {
      dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, static_cast<uintptr_t>(sig), 0,
                                                     dispatch_get_main_queue());
      dispatch_source_set_event_handler(src, ^{
        StopApp();
      });
      dispatch_resume(src);
      sources.push_back(src);
    }

    // Ask for both permissions up front. Without a request macOS neither
    // shows a prompt nor lists the app under Privacy & Security, so there
    // would be no switch to turn on; input would silently be refused and
    // every session view-only. Each request prompts only once per app.
    if (!CGPreflightScreenCaptureAccess()) CGRequestScreenCaptureAccess();
    if (!CGPreflightPostEventAccess()) CGRequestPostEventAccess();
    // The sound's permission too, when sound is on and macOS never asked.
    if (runtime.effective_settings().send_audio && AudioCapturePermission() == 2) RequestAudioCapturePermission();

    RDMMenuBarController* controller = [[RDMMenuBarController alloc] initWithRuntime:&runtime info:info];
    [NSApp run];
    (void)controller;
    for (dispatch_source_t s : sources) dispatch_source_cancel(s);
  }
}

void RunHeadlessApp() {
  @autoreleasepool {
    [NSApplication sharedApplication];
    // Nothing of it shows: no Dock icon, no menu bar, never in front.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    std::vector<dispatch_source_t> sources;
    for (int sig : {SIGINT, SIGTERM}) {
      dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, static_cast<uintptr_t>(sig), 0,
                                                     dispatch_get_main_queue());
      dispatch_source_set_event_handler(src, ^{
        StopApp();
      });
      dispatch_resume(src);
      sources.push_back(src);
    }
    [NSApp run];
    for (dispatch_source_t s : sources) dispatch_source_cancel(s);
  }
}

bool AnotherInstanceRunning() {
  NSString* bundle_id = NSBundle.mainBundle.bundleIdentifier;
  if (bundle_id.length == 0) return false;  // not run from the app bundle
  const pid_t self = NSProcessInfo.processInfo.processIdentifier;
  for (NSRunningApplication* app in [NSRunningApplication runningApplicationsWithBundleIdentifier:bundle_id]) {
    if (app.processIdentifier != self && !app.terminated) return true;
  }
  return false;
}

void ShowStartupError(const std::string& message) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp activateIgnoringOtherApps:YES];
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"RDMachine Server 无法启动";
    a.informativeText = NS(message);
    [a runModal];
  }
}

}  // namespace rdm
