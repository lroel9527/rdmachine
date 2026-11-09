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
#include "rdmachine/session/live_stats.h"

namespace {

NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

NSString* HMS(std::chrono::seconds s) {
  const long long t = s.count();
  return [NSString stringWithFormat:@"%02lld:%02lld:%02lld", t / 3600, t / 60 % 60, t % 60];
}

NSString* Kbps(double kbps) {
  if (kbps >= 1000) return [NSString stringWithFormat:@"%.1f Mbps", kbps / 1000];
  return [NSString stringWithFormat:@"%.0f kbps", kbps];
}

NSString* PeerText(const rdm::MediaHostStatus& st) {
  if (st.origin.method == rdm::AuthMethod::kPairingCode) {
    return [NSString stringWithFormat:@"%@（配对码）", NS(st.origin.peer_ip)];
  }
  return [NSString stringWithFormat:@"%@@%@", NS(st.origin.username), NS(st.origin.peer_ip)];
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

// ---- floating statistics panel ---------------------------------------------

@interface RDMStatsPanel : NSObject
@property(nonatomic, readonly) NSPanel* panel;
- (void)update:(const rdm::MediaHostStatus&)st stats:(const rdm::LiveStats&)ls port:(uint16_t)port;
@end

@implementation RDMStatsPanel {
  NSGridView* _grid;
  std::vector<NSTextField*> _values;
}

static NSArray<NSString*>* StatsKeys() {
  return @[ @"状态", @"时长", @"通道", @"分辨率", @"帧率", @"码率", @"带宽", @"延迟", @"丢包/重传", @"丢帧", @"编码器" ];
}

- (instancetype)initWithMenu:(NSMenu*)menu {
  if (!(self = [super init])) return nil;
  _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 260, 100)
                                      styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                        backing:NSBackingStoreBuffered
                                          defer:YES];
  _panel.level = NSStatusWindowLevel;
  _panel.opaque = NO;
  _panel.backgroundColor = NSColor.clearColor;
  _panel.hasShadow = YES;
  _panel.alphaValue = 0.85;
  _panel.movableByWindowBackground = YES;
  _panel.hidesOnDeactivate = NO;
  _panel.releasedWhenClosed = NO;
  _panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
  _panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary |
                              NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;
  // Deliberately captured with the screen: the person debugging usually sits
  // at the client and sees the panel in the remote picture.

  NSVisualEffectView* bg = [[NSVisualEffectView alloc] initWithFrame:_panel.contentView.bounds];
  bg.material = NSVisualEffectMaterialHUDWindow;
  bg.blendingMode = NSVisualEffectBlendingModeBehindWindow;
  bg.state = NSVisualEffectStateActive;
  bg.wantsLayer = YES;
  bg.layer.cornerRadius = 10;
  bg.layer.masksToBounds = YES;
  bg.menu = menu;  // right click: hide
  _panel.contentView = bg;

  NSMutableArray* rows = [NSMutableArray array];
  NSFont* keyFont = [NSFont systemFontOfSize:11];
  NSFont* valueFont = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightMedium];
  for (NSString* k in StatsKeys()) {
    NSTextField* key = [NSTextField labelWithString:k];
    key.font = keyFont;
    key.textColor = NSColor.secondaryLabelColor;
    NSTextField* value = [NSTextField labelWithString:@"–"];
    value.font = valueFont;
    value.textColor = NSColor.labelColor;
    _values.push_back(value);
    [rows addObject:@[ key, value ]];
  }
  _grid = [NSGridView gridViewWithViews:rows];
  _grid.rowSpacing = 3;
  _grid.columnSpacing = 10;
  [_grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
  _grid.translatesAutoresizingMaskIntoConstraints = NO;
  [bg addSubview:_grid];
  [NSLayoutConstraint activateConstraints:@[
    [_grid.leadingAnchor constraintEqualToAnchor:bg.leadingAnchor constant:12],
    [_grid.trailingAnchor constraintEqualToAnchor:bg.trailingAnchor constant:-12],
    [_grid.topAnchor constraintEqualToAnchor:bg.topAnchor constant:10],
    [_grid.bottomAnchor constraintEqualToAnchor:bg.bottomAnchor constant:-10],
  ]];

  [_panel setFrameAutosaveName:@"RDMStatsPanel"];
  if (![_panel setFrameUsingName:@"RDMStatsPanel"]) {
    const NSRect vis = NSScreen.mainScreen.visibleFrame;
    [_panel setFrameTopLeftPoint:NSMakePoint(NSMaxX(vis) - 280, NSMaxY(vis) - 12)];
  }
  return self;
}

- (void)setValue:(NSString*)text row:(size_t)row {
  _values[row].stringValue = text ?: @"–";
}

- (void)update:(const rdm::MediaHostStatus&)st stats:(const rdm::LiveStats&)ls port:(uint16_t)port {
  const bool live = st.running;
  for (NSInteger i = 1; i < _grid.numberOfRows; ++i) [_grid rowAtIndex:i].hidden = !live;
  if (!live) {
    [self setValue:(port ? [NSString stringWithFormat:@"等待连接 · 端口 %u", port] : @"服务未运行") row:0];
  } else {
    [self setValue:[NSString stringWithFormat:@"%@%@%@", PeerText(st), st.media_connected ? @"" : @" · 等待画面",
                                              st.view_only ? @" · 只看" : @""]
               row:0];
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - st.started);
    [self setValue:HMS(secs) row:1];
    [self setValue:(st.transport == rdm::MediaTransport::kUdp ? @"UDP" : @"TCP（UDP 不通时的备用）") row:2];
    [self setValue:[NSString stringWithFormat:@"%d × %d", st.width, st.height] row:3];
    if (ls.valid) {
      [self setValue:[NSString stringWithFormat:@"%.1f fps（采集 %.1f）", ls.encoded_fps, ls.captured_fps] row:4];
      [self setValue:(ls.target_kbps > 0 ? [NSString stringWithFormat:@"%@（目标 %@）", Kbps(ls.bitrate_kbps),
                                                                      Kbps(ls.target_kbps)]
                                         : Kbps(ls.bitrate_kbps))
                 row:5];
    }
    [self setValue:(ls.bandwidth_kbps > 0 ? Kbps(ls.bandwidth_kbps) : @"未测量（TCP）") row:6];
    [self setValue:(ls.rtt_ms > 0 ? [NSString stringWithFormat:@"%.1f ms", ls.rtt_ms] : @"未测量（TCP）") row:7];
    [self setValue:(ls.recent_loss_pct >= 0 ? [NSString stringWithFormat:@"%.2f%%（累计 %.2f%%）", ls.recent_loss_pct,
                                                                         ls.total_loss_pct]
                                            : @"未测量（TCP）")
               row:8];
    [self setValue:[NSString stringWithFormat:@"拥塞 %llu · 关键帧 %llu",
                                              static_cast<unsigned long long>(ls.frames_dropped_congestion),
                                              static_cast<unsigned long long>(ls.keyframes)]
               row:9];
    [self setValue:(st.encoder.empty() ? @"–" : NS(st.encoder)) row:10];
  }
  // Grow or shrink with the text, keeping the top left corner in place.
  const NSPoint topLeft = NSMakePoint(NSMinX(_panel.frame), NSMaxY(_panel.frame));
  [_panel setContentSize:_panel.contentView.fittingSize];
  [_panel setFrameTopLeftPoint:topLeft];
  [_panel invalidateShadow];  // else the old outline lingers
}

@end

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
  NSButton* _overlay;
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
  _overlay = [NSButton checkboxWithTitle:@"显示调试浮窗（连接时间、通道、帧率、延迟、丢包）" target:nil action:nil];
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
    @[ [NSTextField labelWithString:@"调试："], _overlay ],
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
  _overlay.state = s.show_stats_overlay ? NSControlStateValueOn : NSControlStateValueOff;
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
  s.show_stats_overlay = _overlay.state == NSControlStateValueOn;
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
  RDMStatsPanel* _stats;
  RDMSettingsWindow* _settings;
  NSTimer* _timer;
  rdm::LiveStatsSampler _sampler;
  std::chrono::steady_clock::time_point _sessionStarted;
  bool _wasRunning;
}

- (instancetype)initWithRuntime:(rdm::ServerRuntime*)runtime info:(const rdm::MenuBarAppInfo&)info {
  if (!(self = [super init])) return nil;
  _runtime = runtime;
  _info = info;
  _wasRunning = false;
  _item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
  _item.button.toolTip = @"RDMachine Server";
  _menu = [[NSMenu alloc] init];
  _menu.delegate = self;
  _menu.autoenablesItems = NO;
  _item.menu = _menu;

  NSMenu* panelMenu = [[NSMenu alloc] init];
  [panelMenu addItemWithTitle:@"隐藏调试浮窗" action:@selector(toggleStats:) keyEquivalent:@""].target = self;
  _stats = [[RDMStatsPanel alloc] initWithMenu:panelMenu];

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
  if (st.running && (!_wasRunning || st.started != _sessionStarted)) {
    _sampler.Reset();
    _sessionStarted = st.started;
  }
  _wasRunning = st.running;
  const rdm::LiveStats ls = st.running ? _sampler.Update(st.stats, std::chrono::steady_clock::now()) : rdm::LiveStats{};

  NSString* symbol = st.running ? @"rectangle.inset.filled.and.person.filled" : @"display";
  NSImage* img = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:@"RDMachine Server"];
  [img setTemplate:YES];  // not img.template: a C++ keyword in Objective-C++
  _item.button.image = img;
  _item.button.title = img ? @"" : (st.running ? @"RD●" : @"RD");

  const bool show = _runtime->saved_settings().show_stats_overlay;
  if (show) {
    [_stats update:st stats:ls port:_runtime->port()];
    if (!_stats.panel.visible) [_stats.panel orderFrontRegardless];
  } else if (_stats.panel.visible) {
    [_stats.panel orderOut:nil];
  }
}

- (NSMenuItem*)add:(NSString*)title action:(SEL)action {
  NSMenuItem* item = [_menu addItemWithTitle:title action:action keyEquivalent:@""];
  item.target = self;
  item.enabled = action != nullptr;
  return item;
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
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
  const bool access = AXIsProcessTrusted();
  [self add:(capture ? @"屏幕录制：已允许" : @"屏幕录制：未允许（点此打开系统设置）")
      action:(capture ? nullptr : @selector(openScreenRecording:))];
  [self add:(access ? @"辅助功能：已允许" : @"辅助功能：未允许，只能看不能控（点此打开系统设置）")
      action:(access ? nullptr : @selector(openAccessibility:))];

  [menu addItem:NSMenuItem.separatorItem];
  NSMenuItem* overlay = [self add:@"显示调试浮窗" action:@selector(toggleStats:)];
  overlay.state = _runtime->saved_settings().show_stats_overlay ? NSControlStateValueOn : NSControlStateValueOff;
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
  [NSWorkspace.sharedWorkspace
      openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"]];
}

- (void)setStatsShown:(bool)show {
  std::string error;
  if (!_runtime->SetShowStatsOverlay(show, &error)) NSLog(@"rdmachine: %s", error.c_str());
  [self tick];
}

- (void)toggleStats:(id)sender {
  [self setStatsShown:!_runtime->saved_settings().show_stats_overlay];
}

- (void)showSettings:(id)sender {
  [_settings showWithSettings:_runtime->saved_settings() overrides:NS(_runtime->overrides().Describe())];
}

- (void)applySettings:(const rdm::ServerSettings&)s {
  rdm::ServerSettings without_overlay = s;
  without_overlay.show_stats_overlay = _runtime->saved_settings().show_stats_overlay;
  if (without_overlay == _runtime->saved_settings()) {
    // At most the panel changed: no need to restart (and drop the connection).
    [self setStatsShown:s.show_stats_overlay];
    return;
  }
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
