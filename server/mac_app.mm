// The server's menu bar icon, settings window and floating statistics panel.
// Everything here runs on the main thread; ServerRuntime is thread-safe.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>

#include <signal.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "mac_app.h"

namespace {

NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

NSString* HMS(std::chrono::seconds s) {
  const long long t = s.count();
  return [NSString stringWithFormat:@"%02lld:%02lld:%02lld", t / 3600, t / 60 % 60, t % 60];
}

NSString* PeerText(const rdm::MediaHostStatus& st) {
  if (st.origin.method == rdm::AuthMethod::kPairingCode) {
    return [NSString stringWithFormat:@"%@（配对码）", NS(st.origin.peer_ip)];
  }
  return [NSString stringWithFormat:@"%@@%@", NS(st.origin.username), NS(st.origin.peer_ip)];
}

// Keyboard input source sync, for the panel and the menu.
NSString* InputSourceText(const rdm::MediaHostStatus& st) {
  if (st.client_input_source.empty()) return @"未同步（客户端没有发来）";
  if (st.input_source.empty()) return [NSString stringWithFormat:@"本机没有对应「%@」的输入法", NS(st.client_input_source)];
  if (st.input_source == st.client_input_source) return NS(st.input_source);
  return [NSString stringWithFormat:@"%@（客户端：%@）", NS(st.input_source), NS(st.client_input_source)];
}

NSString* GroupedCode(const std::string& code) {
  if (code.size() != 8) return NS(code);
  return [NSString stringWithFormat:@"%@ %@", NS(code.substr(0, 4)), NS(code.substr(4))];
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
  NSButton* _pairing;
  NSButton* _viewOnly;
  NSPopUpButton* _clipboard;
  NSTextField* _fps;
  NSTextField* _bitrate;
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
  _pairing = [NSButton checkboxWithTitle:@"允许用配对码登录" target:nil action:nil];
  _viewOnly = [NSButton checkboxWithTitle:@"只看不控（不接受对方的键盘和鼠标）" target:nil action:nil];
  _clipboard = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  for (NSString* t : kClipboardTitles) [_clipboard addItemWithTitle:t];
  _fps = NumberField(0, 240);
  _bitrate = NumberField(0, 1000000);
  _note = [NSTextField wrappingLabelWithString:@""];
  _note.textColor = NSColor.systemOrangeColor;
  _note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];

  NSGridView* grid = [NSGridView gridViewWithViews:@[
    @[ [NSTextField labelWithString:@"端口："], WithHint(_port, @"TCP 控制 + UDP 画面，备用 TCP 用下一个端口") ],
    @[ [NSTextField labelWithString:@"登录："], _pairing ],
    @[ [NSTextField labelWithString:@"控制："], _viewOnly ],
    @[ [NSTextField labelWithString:@"剪贴板："], _clipboard ],
    @[ [NSTextField labelWithString:@"帧率上限："], WithHint(_fps, @"fps，0 = 默认（60）") ],
    @[ [NSTextField labelWithString:@"码率上限："], WithHint(_bitrate, @"kbps，0 = 默认（8000）") ],
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
  _pairing.state = s.offer_pairing_codes ? NSControlStateValueOn : NSControlStateValueOff;
  _viewOnly.state = s.view_only ? NSControlStateValueOn : NSControlStateValueOff;
  for (size_t i = 0; i < 4; ++i) {
    if (kClipboardModes[i] == s.clipboard_mode) [_clipboard selectItemAtIndex:static_cast<NSInteger>(i)];
  }
  _fps.integerValue = s.fps;
  _bitrate.integerValue = s.bitrate_kbps;
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

- (void)save:(id)sender {
  [_window makeFirstResponder:nil];  // commit the field being edited
  rdm::ServerSettings s;
  s.port = static_cast<uint16_t>(std::clamp<NSInteger>(_port.integerValue, 1, 65534));
  s.offer_pairing_codes = _pairing.state == NSControlStateValueOn;
  s.view_only = _viewOnly.state == NSControlStateValueOn;
  s.clipboard_mode = kClipboardModes[std::clamp<NSInteger>(_clipboard.indexOfSelectedItem, 0, 3)];
  s.fps = static_cast<int32_t>(std::clamp<NSInteger>(_fps.integerValue, 0, 240));
  s.bitrate_kbps = static_cast<int32_t>(std::clamp<NSInteger>(_bitrate.integerValue, 0, 1000000));
  if (self.onSave) self.onSave(s);
  [_window close];
}

@end

// ---- menu bar ----------------------------------------------------------------

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
}

- (instancetype)initWithRuntime:(rdm::ServerRuntime*)runtime info:(const rdm::MenuBarAppInfo&)info {
  if (!(self = [super init])) return nil;
  _runtime = runtime;
  _info = info;
  _menuOpen = false;
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

  [self tick];
  // Common modes: keep updating while the menu is open.
  _timer = [NSTimer timerWithTimeInterval:1.0 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
  return self;
}

- (void)tick {
  const rdm::MediaHostStatus st = _runtime->Status();

  // Only when it changes: setting the image relays out the status item,
  // which can swallow a click that is in progress.
  NSString* symbol = st.running ? @"rectangle.inset.filled.and.person.filled" : @"display";
  if (![symbol isEqualToString:_symbol]) {
    _symbol = symbol;
    NSImage* img = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:@"RDMachine Server"];
    [img setTemplate:YES];  // not img.template: a C++ keyword in Objective-C++
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
  [self add:(port ? [NSString stringWithFormat:@"正在监听端口 %u（登录账号：%@）", port, NS(_info.user)]
                  : @"服务未运行")
      action:nullptr];
  if (st.running) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - st.started);
    [self add:[NSString stringWithFormat:@"已连接：%@ · %@ · %@", PeerText(st),
                                         st.transport == rdm::MediaTransport::kUdp ? @"UDP" : @"TCP", HMS(secs)]
        action:nullptr];
    [self add:[NSString stringWithFormat:@"键盘语言：%@", InputSourceText(st)] action:nullptr];
    if (st.input_unavailable) {
      [self add:@"这次连接只能看不能控：没有辅助功能权限" action:@selector(openAccessibility:)];
    }
    [self add:@"断开当前连接" action:@selector(disconnect:)];
  } else {
    [self add:@"无连接" action:nullptr];
  }

  [menu addItem:NSMenuItem.separatorItem];
  if (_runtime->effective_settings().offer_pairing_codes) {
    const auto code = _runtime->CurrentPairingCode();
    if (!code.code.empty()) {
      [self add:[NSString stringWithFormat:@"配对码：%@（还剩 %lld:%02lld，用一次失效）", GroupedCode(code.code),
                                           static_cast<long long>(code.remaining.count() / 60),
                                           static_cast<long long>(code.remaining.count() % 60)]
          action:nullptr];
      [self add:@"复制配对码" action:@selector(copyCode:)];
    }
    [self add:(code.code.empty() ? @"生成配对码" : @"换一个配对码") action:@selector(newCode:)];
  } else {
    [self add:@"配对码登录已关闭" action:nullptr];
  }
  NSMenuItem* fp = [self add:@"复制本机指纹" action:@selector(copyFingerprint:)];
  fp.toolTip = NS(_info.fingerprint);

  [menu addItem:NSMenuItem.separatorItem];
  const bool capture = CGPreflightScreenCaptureAccess();
  // The same check rdaccel makes before it injects input.
  const bool input = CGPreflightPostEventAccess();
  [self add:(capture ? @"屏幕录制：已允许" : @"屏幕录制：未允许（点此打开系统设置）")
      action:(capture ? nullptr : @selector(openScreenRecording:))];
  [self add:(input ? @"辅助功能：已允许" : @"辅助功能：未允许，只能看不能控（点此打开系统设置）")
      action:(input ? nullptr : @selector(openAccessibility:))];
  if (!capture || !input) {
    [self add:@"    在系统设置里打开 RDMachine Server 的开关；下次连接还不行就退出重开" action:nullptr];
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

- (void)newCode:(id)sender {
  const std::string code = _runtime->IssuePairingCode();
  if (!code.empty()) Copy(NS(code));
}

- (void)copyCode:(id)sender {
  const auto code = _runtime->CurrentPairingCode();
  if (!code.code.empty()) Copy(NS(code.code));
}

- (void)copyFingerprint:(id)sender {
  Copy(NS(_info.fingerprint));
}

- (void)openScreenRecording:(id)sender {
  CGRequestScreenCaptureAccess();  // adds the app to the list (asks once)
  [NSWorkspace.sharedWorkspace
      openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"]];
}

- (void)openAccessibility:(id)sender {
  CGRequestPostEventAccess();  // adds the app to the list (asks once)
  [NSWorkspace.sharedWorkspace
      openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"]];
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

    RDMMenuBarController* controller = [[RDMMenuBarController alloc] initWithRuntime:&runtime info:info];
    [NSApp run];
    (void)controller;
    for (dispatch_source_t s : sources) dispatch_source_cancel(s);
  }
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
