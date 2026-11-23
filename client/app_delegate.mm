#include "app_delegate.h"

#include "client_support.h"
#include "remote_window.h"

using namespace rdmclient;

// ---------------------------------------------------------------------------
// Connect window and application

// One computer in the list: an icon tile, the name with a status pill, a
// line of details, and the row's button (Connect / Show window).
@interface MachineCell : NSTableCellView
@property(nonatomic, strong) NSView* tile;
@property(nonatomic, strong) NSImageView* icon;
@property(nonatomic, strong) NSTextField* name;
@property(nonatomic, strong) NSView* pill;
@property(nonatomic, strong) NSTextField* pillText;
@property(nonatomic, strong) NSTextField* detail;
@property(nonatomic, strong) NSButton* action;
@property(nonatomic, strong) NSColor* tint;  // status colour; nil = neutral
- (void)setStatus:(NSString*)text tint:(NSColor*)tint;
@end

@implementation MachineCell

- (instancetype)initWithFrame:(NSRect)frame {
  if (!(self = [super initWithFrame:frame])) return nil;
  _tile = [[NSView alloc] initWithFrame:NSZeroRect];
  _tile.wantsLayer = YES;
  _tile.layer.cornerRadius = 10;
  [self addSubview:_tile];
  _icon = [[NSImageView alloc] initWithFrame:NSZeroRect];
  _icon.image = [NSImage imageWithSystemSymbolName:@"desktopcomputer" accessibilityDescription:nil];
  _icon.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:18 weight:NSFontWeightRegular];
  [_tile addSubview:_icon];
  _name = [NSTextField labelWithString:@""];
  _name.font = [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];
  _name.lineBreakMode = NSLineBreakByTruncatingTail;
  [self addSubview:_name];
  _pill = [[NSView alloc] initWithFrame:NSZeroRect];
  _pill.wantsLayer = YES;
  _pill.layer.cornerRadius = 8;
  [self addSubview:_pill];
  _pillText = [NSTextField labelWithString:@""];
  _pillText.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
  [_pill addSubview:_pillText];
  _detail = [NSTextField labelWithString:@""];
  _detail.font = [NSFont systemFontOfSize:12];
  _detail.textColor = NSColor.secondaryLabelColor;
  _detail.lineBreakMode = NSLineBreakByTruncatingTail;
  [self addSubview:_detail];
  _action = [NSButton buttonWithTitle:@"连接" target:nil action:nil];
  _action.bezelStyle = NSBezelStyleRounded;
  _action.controlSize = NSControlSizeRegular;
  [self addSubview:_action];
  return self;
}

- (void)setStatus:(NSString*)text tint:(NSColor*)tint {
  _tint = tint;
  _pillText.stringValue = text ?: @"";
  _pill.hidden = text.length == 0;
  [self applyColors];
  self.needsLayout = YES;
}

// Layer colours do not follow light and dark mode by themselves.
- (void)applyColors {
  [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
    NSColor* base = self->_tint ?: NSColor.secondaryLabelColor;
    self->_tile.layer.backgroundColor = [base colorWithAlphaComponent:self->_tint ? 0.16 : 0.10].CGColor;
    self->_icon.contentTintColor = base;
    self->_pill.layer.backgroundColor = [base colorWithAlphaComponent:0.16].CGColor;
    self->_pillText.textColor = base;
  }];
}

- (void)viewDidChangeEffectiveAppearance {
  [super viewDidChangeEffectiveAppearance];
  [self applyColors];
}

- (void)layout {
  [super layout];
  const CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
  _tile.frame = NSMakeRect(12, std::round((H - 40) / 2), 40, 40);
  _icon.frame = NSMakeRect(8, 8, 24, 24);
  [_action sizeToFit];
  NSRect b = _action.frame;
  b.size.width = std::max<CGFloat>(b.size.width, 84);
  b.origin = NSMakePoint(W - b.size.width - 12, std::round((H - b.size.height) / 2));
  _action.frame = b;
  const CGFloat left = 64, right = b.origin.x - 12;
  // Measured from the text itself: a reused cell's intrinsic size can be the
  // previous row's.
  const NSSize pillSize = NSMakeSize(std::ceil(_pillText.attributedStringValue.size.width) + 2, 15);
  const CGFloat pillW = _pill.hidden ? 0 : pillSize.width + 14;
  const CGFloat textW = std::ceil(_name.attributedStringValue.size.width) + 4;
  const CGFloat nameW = std::min<CGFloat>(textW, right - left - pillW - (pillW ? 8 : 0));
  _name.frame = NSMakeRect(left, H / 2 + 1, std::max<CGFloat>(nameW, 0), 19);
  _pill.frame = NSMakeRect(left + nameW + 8, H / 2 + 2, pillW, 17);
  _pillText.frame = NSMakeRect(7, 1, pillSize.width, 15);
  _detail.frame = NSMakeRect(left, H / 2 - 19, std::max<CGFloat>(right - left, 0), 16);
}

@end

// One connection being made. Several can be under way at once (a window
// connecting again after a drop while the user connects to another
// computer); each carries what it needs, nothing is read back from the form.
@interface ConnectAttempt : NSObject {
 @public
  std::string key, user, password;  // remembered once it works (SaveLogin)
}
@property(nonatomic, strong) RemoteWindowController* remote;
@property(nonatomic) BOOL reused;     // into a window that is already open (a reconnect)
@property(nonatomic) BOOL automatic;  // with a saved login rather than from the form
@property(nonatomic) BOOL cancelled;  // the user gave up on it: whatever comes back is dropped
@property(nonatomic, copy) NSString* target;
@end

@implementation ConnectAttempt
- (void)dealloc {
  std::fill(password.begin(), password.end(), '\0');
}
@end

@interface AppDelegate ()
- (RemoteWindowController*)windowForTarget:(NSString*)target;
- (RemoteWindowController*)frontRemote;
@end

@implementation AppDelegate {
  // The main window: the computers connected to before.
  NSWindow* _machinesWindow;
  NSTableView* _machinesTable;
  NSButton* _machinesConnect;
  NSProgressIndicator* _machinesSpinner;
  NSArray<NSDictionary*>* _machines;
  // What the table shows: the computers, with a header row (@{@"header":
  // name, @"count": n}) before each group when the list is grouped.
  NSArray<NSDictionary*>* _rows;
  NSButton* _groupToggle;
  NSTextField* _machinesSummary;
  NSScrollView* _machinesScroll;
  // A bar at the top of the list after a connection ended or failed (instead
  // of a dialog): what happened, reconnect, dismiss.
  NSView* _notice;
  NSImageView* _noticeIcon;
  NSTextField* _noticeTitle;
  NSTextField* _noticeDetail;
  NSButton* _noticeAction;
  NSString* _noticeTarget;
  // Which listed computers answered on their control port lately (target ->
  // @YES/@NO); probed every 15 seconds while the app runs.
  NSMutableDictionary<NSString*, NSNumber*>* _online;
  NSMutableSet<NSString*>* _probing;
  NSTimer* _probeTimer;
  NSTimer* _refreshTimer;
  // The form for a new computer (or one whose password is not saved).
  NSWindow* _connectWindow;
  NSTextField* _host;
  NSTextField* _user;
  NSSecureTextField* _password;
  NSButton* _connectButton;
  NSProgressIndicator* _spinner;
  // One window per connected computer (several at once are fine), and the
  // connections being made.
  NSMutableArray<RemoteWindowController*>* _remotes;
  NSMutableArray<ConnectAttempt*>* _attempts;
  std::unique_ptr<rdm::KnownHosts> _knownHosts;
  // The menu bar icon: the connections, and a ring while files pasted here
  // are on their way (Finder shows nothing until they are all here).
  NSStatusItem* _statusItem;
  NSMenu* _statusMenu;
  NSTimer* _statusTimer;
  BOOL _statusMenuOpen;
  NSString* _statusSymbol;  // the icon now shown (nil: the ring)
  int _ringStep;            // -1: no ring; 0..40, or 41 when the size is unknown
}

static NSTextField* Label(NSString* text, NSRect frame) {
  NSTextField* l = [NSTextField labelWithString:text];
  l.frame = frame;
  l.alignment = NSTextAlignmentRight;
  return l;
}

// Developer aid: rdmachine-client --snapshot=DIR draws the list (with sample
// computers) and the new-connection form into DIR/list.png and DIR/form.png
// without showing a window, then quits. For checking the layout.
- (void)writeSnapshotsTo:(NSString*)dir {
  _machines = @[
    @{@"target" : @"10.0.0.3", @"user" : @"me", @"name" : @"Mac mini", @"last" : NSDate.date},
    @{@"target" : @"192.168.1.20", @"user" : @"me", @"name" : @"Office-iMac",
      @"last" : [NSDate dateWithTimeIntervalSinceNow:-3 * 86400]},
    @{@"target" : @"home.example", @"user" : @"", @"name" : @"家里的 Mac",
      @"last" : [NSDate dateWithTimeIntervalSinceNow:-14 * 86400]},
  ];
  _online[@"192.168.1.20"] = @YES;
  _online[@"home.example"] = @NO;
  _rows = _machines;
  [_machinesTable reloadData];
  [self updateMachinesSummary];
  // The once-a-second refresh must keep the selection.
  [_machinesTable selectRowIndexes:[NSIndexSet indexSetWithIndex:1] byExtendingSelection:NO];
  for (int i = 0; i < 3; ++i) [self refreshRows];
  fprintf(stderr, "snapshot: selected row after refresh = %ld\n", static_cast<long>(_machinesTable.selectedRow));
  auto write = [](NSWindow* w, NSString* path) {
    NSView* v = w.contentView.superview ?: w.contentView;
    [v layoutSubtreeIfNeeded];
    [v displayIfNeeded];
    NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
  };
  write(_machinesWindow, [dir stringByAppendingPathComponent:@"list.png"]);
  {
    // Aliases and groups, the list grouped.
    NSMutableArray* grouped = [NSMutableArray array];
    for (NSDictionary* m in _machines) {
      NSMutableDictionary* x = [m mutableCopy];
      if ([m[@"target"] isEqualToString:@"10.0.0.3"]) {
        x[@"alias"] = @"书房";
        x[@"group"] = @"家里";
      }
      if ([m[@"target"] isEqualToString:@"192.168.1.20"]) x[@"group"] = @"公司";
      [grouped addObject:x];
    }
    NSArray* saved = _machines;
    _machines = grouped;
    const bool was = MachinesGrouped();
    [NSUserDefaults.standardUserDefaults setBool:YES forKey:kMachinesGroupedKey];
    [self buildRows];
    _groupToggle.state = NSControlStateValueOn;
    [_machinesTable reloadData];
    write(_machinesWindow, [dir stringByAppendingPathComponent:@"list-groups.png"]);
    [NSUserDefaults.standardUserDefaults setBool:was forKey:kMachinesGroupedKey];
    _machines = saved;
    _rows = saved;
    _groupToggle.state = NSControlStateValueOff;
    [_machinesTable reloadData];
  }
  [self showNotice:@"与 Mac mini 的连接已断开" detail:@"那台 Mac 的服务停止了。" target:@"10.0.0.3" warning:NO];
  write(_machinesWindow, [dir stringByAppendingPathComponent:@"list-notice.png"]);
  RemoteWindowController* demo = [[RemoteWindowController alloc] initWithTitle:@"Mac mini（10.0.0.3）"];
  [demo.window setContentSize:NSMakeSize(900, 560)];
  [demo showStatus:@"正在重新连接"
            detail:@"和那台 Mac 的连接断了，正在自动重新连接。"
            symbol:@"arrow.clockwise"
             until:[NSDate dateWithTimeIntervalSinceNow:52]
          canRetry:YES];
  write(demo.window, [dir stringByAppendingPathComponent:@"status.png"]);
  [demo hideStatus];
  write(_connectWindow, [dir stringByAppendingPathComponent:@"form.png"]);
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  _remotes = [NSMutableArray array];
  _attempts = [NSMutableArray array];
  _knownHosts = std::make_unique<rdm::KnownHosts>(rdm::DefaultConfigDir() + "/known_hosts");
  [self buildMenu];
  [self buildStatusItem];
  _connectWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 440, 244)
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
  _connectWindow.title = @"新建连接";
  _connectWindow.releasedWhenClosed = NO;
  // No zoom-in animation: it showed a small translucent frame before the
  // window, which looked like a stray window flashing at start.
  _connectWindow.animationBehavior = NSWindowAnimationBehaviorNone;
  NSView* v = _connectWindow.contentView;
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;

  NSTextField* heading = [NSTextField labelWithString:@"连接到一台 Mac"];
  heading.font = [NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
  heading.frame = NSMakeRect(24, 196, 392, 24);
  [v addSubview:heading];

  [v addSubview:Label(@"电脑", NSMakeRect(20, 156, 90, 20))];
  _host = [NSTextField textFieldWithString:[defaults stringForKey:@"lastHost"] ?: @""];
  _host.frame = NSMakeRect(120, 153, 296, 24);
  _host.placeholderString = @"mac.local 或 192.168.1.20";
  [v addSubview:_host];

  [v addSubview:Label(@"账号", NSMakeRect(20, 120, 90, 20))];
  _user = [NSTextField textFieldWithString:[defaults stringForKey:@"lastUser"] ?: NSUserName()];
  _user.frame = NSMakeRect(120, 117, 296, 24);
  [v addSubview:_user];

  [v addSubview:Label(@"密码", NSMakeRect(20, 84, 90, 20))];
  _password = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(120, 81, 296, 24)];
  _password.placeholderString = @"那台 Mac 上这个账号的登录密码";
  [v addSubview:_password];

  NSTextField* hint = [NSTextField labelWithString:@"连上后会记住，下次在列表里双击就能连。"];
  hint.font = [NSFont systemFontOfSize:12];
  hint.textColor = NSColor.secondaryLabelColor;
  hint.frame = NSMakeRect(120, 54, 296, 18);
  [v addSubview:hint];

  _spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(24, 20, 20, 20)];
  _spinner.style = NSProgressIndicatorStyleSpinning;
  _spinner.displayedWhenStopped = NO;
  [v addSubview:_spinner];
  NSButton* cancel = [NSButton buttonWithTitle:@"取消" target:_connectWindow action:@selector(performClose:)];
  cancel.frame = NSMakeRect(222, 14, 96, 32);
  cancel.keyEquivalent = @"\e";
  [v addSubview:cancel];
  _connectButton = [NSButton buttonWithTitle:@"连接" target:self action:@selector(connect:)];
  _connectButton.frame = NSMakeRect(322, 14, 96, 32);
  _connectButton.keyEquivalent = @"\r";
  [v addSubview:_connectButton];

  [_connectWindow center];
  _connectWindow.delegate = self;  [_connectWindow center];
  _connectWindow.delegate = self;  // closing the form goes back to the list
  [self buildMachinesWindow];
  for (NSString* a in NSProcessInfo.processInfo.arguments) {
    if ([a hasPrefix:@"--snapshot="]) {
      [self writeSnapshotsTo:[a substringFromIndex:11]];
      [NSApp terminate:nil];
      return;
    }
  }
  [NSApp activateIgnoringOtherApps:YES];

  // rdmachine-client HOST[:PORT]: the address filled in. A password is
  // never taken from the command line.
  NSString* host = nil;
  NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
  for (NSUInteger i = 1; i < args.count; ++i) {
    NSString* a = args[i];
    if ([a hasPrefix:@"-"]) {
      // "-Key value" pairs are NSUserDefaults' (the argument domain).
      if (![a hasPrefix:@"--"]) ++i;
    } else if (!host) {
      host = a;
    }
  }
  BOOL started = NO;
  if (host) {
    _host.stringValue = host;
  } else if (!(NSEvent.modifierFlags & NSEventModifierFlagOption) && ![self anotherCopyRunning]) {
    // Straight back to the last computer with its remembered login; hold
    // Option while opening the app to get the list instead.
    NSArray* machines = LoadMachines();
    if (machines.count > 0) started = [self connectWithSavedLoginTo:machines[0][@"target"] remote:nil];
  }
  if (!started) [self showMachines];
}

// Another copy of this app runs already (opened with open -n, say): it has
// the last computer, so this one starts at the list rather than taking that
// session over.
- (BOOL)anotherCopyRunning {
  NSString* bundle_id = NSBundle.mainBundle.bundleIdentifier;
  if (bundle_id.length == 0) return NO;
  const pid_t me = NSProcessInfo.processInfo.processIdentifier;
  for (NSRunningApplication* app in [NSRunningApplication runningApplicationsWithBundleIdentifier:bundle_id]) {
    if (app.processIdentifier != me && !app.terminated) return YES;
  }
  return NO;
}

// The list; the new-computer form when there is nothing in it yet.
- (void)showMachines {
  [self reloadMachines];
  if (_machines.count == 0) {
    [self showConnectWindow];
    return;
  }
  [_connectWindow orderOut:nil];
  [_machinesWindow makeKeyAndOrderFront:nil];
  [_machinesWindow makeFirstResponder:_machinesTable];
}

// The form, for a new computer or one whose password is needed.
- (void)showConnectWindow {
  [_connectWindow makeKeyAndOrderFront:nil];
  // Not the password field: macOS shows its Passwords AutoFill popup under a
  // focused password field, which flashed up with the window at every start.
  if (_connectWindow.firstResponder != _password) [_connectWindow makeFirstResponder:_host];
}

- (void)newConnection:(id)sender {
  _host.stringValue = @"";
  _user.stringValue = NSUserName();
  _password.stringValue = @"";
  [self showConnectWindow];
  [_connectWindow makeFirstResponder:_host];
}

- (void)windowWillClose:(NSNotification*)note {
  if (note.object != _connectWindow) return;
  // The form closed without connecting (Cancel): a connection it started is
  // given up, and it is back to the list.
  for (ConnectAttempt* a in _attempts) {
    if (!a.reused) a.cancelled = YES;
  }
  [self updateBusy];
  if (LoadMachines().count > 0) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self showMachines];
    });
  }
}

- (void)buildMachinesWindow {
  _machinesWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 420)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                          NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
  _machinesWindow.title = @"rdmachine";
  _machinesWindow.releasedWhenClosed = NO;
  _machinesWindow.animationBehavior = NSWindowAnimationBehaviorNone;
  _machinesWindow.contentMinSize = NSMakeSize(440, 260);
  _machinesWindow.frameAutosaveName = @"MachinesWindow";
  NSView* v = _machinesWindow.contentView;

  // "新建连接" at the right of the title bar.
  NSButton* add = [NSButton buttonWithTitle:@"新建连接" target:self action:@selector(newConnection:)];
  add.image = [NSImage imageWithSystemSymbolName:@"plus" accessibilityDescription:nil];
  add.imagePosition = NSImageLeading;
  add.bezelStyle = NSBezelStyleRounded;
  add.controlSize = NSControlSizeSmall;
  add.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
  [add sizeToFit];
  NSView* accessory = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, add.frame.size.width + 12, 28)];
  [add setFrameOrigin:NSMakePoint(0, std::round((28 - add.frame.size.height) / 2))];
  [accessory addSubview:add];
  NSTitlebarAccessoryViewController* acc = [[NSTitlebarAccessoryViewController alloc] init];
  acc.view = accessory;
  acc.layoutAttribute = NSLayoutAttributeTrailing;
  [_machinesWindow addTitlebarAccessoryViewController:acc];

  NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 32, 560, 388)];
  _machinesScroll = scroll;
  scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  scroll.hasVerticalScroller = YES;
  scroll.borderType = NSNoBorder;
  scroll.drawsBackground = NO;
  _machinesTable = [[NSTableView alloc] initWithFrame:scroll.bounds];
  NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"machine"];
  col.resizingMask = NSTableColumnAutoresizingMask;
  col.width = scroll.contentSize.width;  // one column, as wide as the list
  [_machinesTable addTableColumn:col];
  _machinesTable.headerView = nil;
  _machinesTable.rowHeight = 64;
  _machinesTable.intercellSpacing = NSMakeSize(0, 2);
  _machinesTable.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
  _machinesTable.backgroundColor = NSColor.clearColor;
  _machinesTable.style = NSTableViewStyleInset;
  _machinesTable.dataSource = self;
  _machinesTable.delegate = self;
  _machinesTable.target = self;
  _machinesTable.doubleAction = @selector(connectSelected:);
  NSMenu* menu = [[NSMenu alloc] init];
  [menu addItemWithTitle:@"连接" action:@selector(connectSelected:) keyEquivalent:@""].target = self;
  [menu addItemWithTitle:@"编辑…" action:@selector(editMachine:) keyEquivalent:@""].target = self;
  [menu addItemWithTitle:@"忘记密码" action:@selector(forgetMachinePassword:) keyEquivalent:@""].target = self;
  [menu addItem:NSMenuItem.separatorItem];
  [menu addItemWithTitle:@"从列表中删除" action:@selector(removeMachine:) keyEquivalent:@""].target = self;
  _machinesTable.menu = menu;
  scroll.documentView = _machinesTable;
  [v addSubview:scroll];
  [_machinesTable sizeLastColumnToFit];

  NSBox* line = [[NSBox alloc] initWithFrame:NSMakeRect(0, 31, 560, 1)];
  line.boxType = NSBoxSeparator;
  line.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
  [v addSubview:line];
  _machinesSummary = [NSTextField labelWithString:@""];
  _machinesSummary.font = [NSFont systemFontOfSize:11];
  _machinesSummary.textColor = NSColor.secondaryLabelColor;
  _machinesSummary.frame = NSMakeRect(16, 8, 200, 16);
  _machinesSummary.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
  [v addSubview:_machinesSummary];
  _groupToggle = [NSButton checkboxWithTitle:@"按分组显示" target:self action:@selector(toggleGrouped:)];
  _groupToggle.controlSize = NSControlSizeSmall;
  _groupToggle.font = [NSFont systemFontOfSize:11];
  _groupToggle.frame = NSMakeRect(214, 6, 110, 20);
  _groupToggle.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
  [v addSubview:_groupToggle];
  NSTextField* tips = [NSTextField labelWithString:@"双击连接 · 右键改别名、分组"];
  tips.font = [NSFont systemFontOfSize:11];
  tips.textColor = NSColor.tertiaryLabelColor;
  tips.alignment = NSTextAlignmentRight;
  tips.frame = NSMakeRect(330, 8, 214, 16);
  tips.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
  [v addSubview:tips];

  // The notice bar (hidden until something happens).
  _notice = [[NSView alloc] initWithFrame:NSMakeRect(12, 420 - 70, 536, 62)];
  _notice.wantsLayer = YES;
  _notice.layer.cornerRadius = 10;
  _notice.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
  _notice.hidden = YES;
  _noticeIcon = [[NSImageView alloc] initWithFrame:NSMakeRect(14, 17, 28, 28)];
  _noticeIcon.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:20 weight:NSFontWeightMedium];
  [_notice addSubview:_noticeIcon];
  _noticeTitle = [NSTextField labelWithString:@""];
  _noticeTitle.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
  _noticeTitle.lineBreakMode = NSLineBreakByTruncatingTail;
  _noticeTitle.frame = NSMakeRect(52, 33, 330, 18);
  _noticeTitle.autoresizingMask = NSViewWidthSizable;
  [_notice addSubview:_noticeTitle];
  _noticeDetail = [NSTextField labelWithString:@""];
  _noticeDetail.font = [NSFont systemFontOfSize:12];
  _noticeDetail.textColor = NSColor.secondaryLabelColor;
  _noticeDetail.lineBreakMode = NSLineBreakByTruncatingTail;
  _noticeDetail.frame = NSMakeRect(52, 13, 330, 16);
  _noticeDetail.autoresizingMask = NSViewWidthSizable;
  [_notice addSubview:_noticeDetail];
  _noticeAction = [NSButton buttonWithTitle:@"重新连接" target:self action:@selector(noticeReconnect:)];
  _noticeAction.frame = NSMakeRect(392, 15, 100, 30);
  _noticeAction.autoresizingMask = NSViewMinXMargin;
  [_notice addSubview:_noticeAction];
  NSButton* close = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"xmark" accessibilityDescription:@"关闭"]
                                       target:self
                                       action:@selector(hideNotice:)];
  close.bordered = NO;
  close.frame = NSMakeRect(500, 19, 24, 24);
  close.autoresizingMask = NSViewMinXMargin;
  [_notice addSubview:close];
  [v addSubview:_notice];

  _machinesSpinner = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];  // kept for setBusy
  _machinesConnect = [NSButton buttonWithTitle:@"连接" target:self action:@selector(connectSelected:)];
  _machinesConnect.keyEquivalent = @"\r";  // Return connects the selected row
  _machinesConnect.frame = NSMakeRect(-200, -200, 10, 10);
  [v addSubview:_machinesConnect];
  [_machinesWindow center];

  _online = [NSMutableDictionary dictionary];
  _probing = [NSMutableSet set];
  _probeTimer = [NSTimer scheduledTimerWithTimeInterval:15 target:self selector:@selector(probeMachines) userInfo:nil repeats:YES];
  _refreshTimer = [NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(refreshMachines) userInfo:nil repeats:YES];
  [self probeMachines];
}

// Every 15 seconds: which listed computers (not connected right now) answer.
- (void)probeMachines {
  for (NSDictionary* m in LoadMachines()) {
    NSString* target = m[@"target"];
    if ([self windowForTarget:target] || [_probing containsObject:target]) continue;
    std::string host;
    uint16_t port = 0;
    if (!ParseTarget(target, &host, &port)) continue;
    [_probing addObject:target];
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      const bool up = TcpReachable(host, port, 1500);
      dispatch_async(dispatch_get_main_queue(), ^{
        AppDelegate* me = weakSelf;
        if (!me) return;
        [me->_probing removeObject:target];
        me->_online[target] = @(up);
        [me refreshMachines];
      });
    });
  }
}

// The list's live parts (durations, statuses), once a second while it shows.
- (void)refreshMachines {
  if (!_machinesWindow.visible) return;
  [self refreshRows];
}

- (void)refreshRows {
  // In place: reloadData would rebuild the rows every second, which drops a
  // click in progress and the selection with it.
  for (NSInteger row = 0; row < _machinesTable.numberOfRows; ++row) {
    if (![self machineAtRow:row]) continue;  // group headers do not change
    if (auto* cell = static_cast<MachineCell*>([_machinesTable viewAtColumn:0 row:row makeIfNecessary:NO])) {
      [self configureCell:cell row:row];
    }
  }
  [self updateMachinesSummary];
}

- (void)updateMachinesSummary {
  int live = 0, retrying = 0;
  for (RemoteWindowController* r in _remotes) {
    if (r.isLive) ++live;
    else ++retrying;
  }
  NSMutableArray* parts = [NSMutableArray array];
  [parts addObject:[NSString stringWithFormat:@"%lu 台电脑", static_cast<unsigned long>(_machines.count)]];
  if (live) [parts addObject:[NSString stringWithFormat:@"已连接 %d 台", live]];
  if (retrying) [parts addObject:[NSString stringWithFormat:@"正在重连 %d 台", retrying]];
  _machinesSummary.stringValue = [parts componentsJoinedByString:@" · "];
}

// The notice bar: `target` (may be nil) is what "重新连接" connects to.
- (void)showNotice:(NSString*)title detail:(NSString*)detail target:(NSString*)target warning:(BOOL)warning {
  _noticeTarget = target;
  _noticeTitle.stringValue = title ?: @"";
  _noticeDetail.stringValue = detail ?: @"";
  _noticeDetail.toolTip = detail;
  _noticeAction.hidden = target.length == 0;
  NSColor* tint = warning ? NSColor.systemOrangeColor : NSColor.systemRedColor;
  _noticeIcon.image = [NSImage imageWithSystemSymbolName:warning ? @"exclamationmark.triangle.fill" : @"exclamationmark.circle.fill"
                                accessibilityDescription:nil];
  _noticeIcon.contentTintColor = tint;
  [_notice.effectiveAppearance performAsCurrentDrawingAppearance:^{
    self->_notice.layer.backgroundColor = [tint colorWithAlphaComponent:0.12].CGColor;
    self->_notice.layer.borderColor = [tint colorWithAlphaComponent:0.35].CGColor;
  }];
  _notice.layer.borderWidth = 0.5;
  const NSSize c = _machinesWindow.contentView.bounds.size;
  _notice.frame = NSMakeRect(12, c.height - 70, c.width - 24, 62);
  _notice.hidden = NO;
  _machinesScroll.frame = NSMakeRect(0, 32, c.width, c.height - 32 - 76);
  [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(hideNotice:) object:nil];
  [self performSelector:@selector(hideNotice:) withObject:nil afterDelay:60];
}

- (void)hideNotice:(id)sender {
  if (_notice.hidden) return;
  _notice.hidden = YES;
  const NSSize c = _machinesWindow.contentView.bounds.size;
  _machinesScroll.frame = NSMakeRect(0, 32, c.width, c.height - 32);
}

- (void)noticeReconnect:(id)sender {
  NSString* target = _noticeTarget;
  [self hideNotice:nil];
  if (target.length == 0 || [self connectingTo:target]) return;
  if (RemoteWindowController* open = [self windowForTarget:target]) {
    [open.window makeKeyAndOrderFront:nil];
    return;
  }
  if (![self connectWithSavedLoginTo:target remote:nil]) {
    _host.stringValue = target;
    [self showConnectWindow];
  }
}

// The row's button: the window of a connected computer, or connect.
- (void)rowAction:(NSButton*)sender {
  const NSInteger row = [_machinesTable rowForView:sender];
  if (![self machineAtRow:row]) return;
  [_machinesTable selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];
  [self connectSelected:nil];
}

- (void)reloadMachines {
  _machines = LoadMachines();
  [self buildRows];
  [_machinesTable reloadData];
  [self updateMachinesSummary];
  if (_machinesTable.selectedRow < 0) {
    for (NSUInteger i = 0; i < _rows.count; ++i) {
      if (!_rows[i][@"header"]) {
        [_machinesTable selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
        break;
      }
    }
  }
  _groupToggle.state = MachinesGrouped() ? NSControlStateValueOn : NSControlStateValueOff;
}

// Flat, most recent first; or grouped: groups by name, "未分组" last, each
// most recent first.
- (void)buildRows {
  if (!MachinesGrouped()) {
    _rows = _machines;
    return;
  }
  NSMutableDictionary<NSString*, NSMutableArray*>* groups = [NSMutableDictionary dictionary];
  NSMutableArray* ungrouped = [NSMutableArray array];
  for (NSDictionary* m in _machines) {
    NSString* g = [m[@"group"] length] ? m[@"group"] : nil;
    if (!g) {
      [ungrouped addObject:m];
      continue;
    }
    if (!groups[g]) groups[g] = [NSMutableArray array];
    [groups[g] addObject:m];
  }
  NSMutableArray* rows = [NSMutableArray array];
  for (NSString* g in [groups.allKeys sortedArrayUsingSelector:@selector(localizedStandardCompare:)]) {
    [rows addObject:@{@"header" : g, @"count" : @(groups[g].count)}];
    [rows addObjectsFromArray:groups[g]];
  }
  if (ungrouped.count) {
    [rows addObject:@{@"header" : @"未分组", @"count" : @(ungrouped.count)}];
    [rows addObjectsFromArray:ungrouped];
  }
  _rows = rows;
}

- (NSDictionary*)machineAtRow:(NSInteger)row {
  if (row < 0 || static_cast<NSUInteger>(row) >= _rows.count) return nil;
  NSDictionary* r = _rows[static_cast<NSUInteger>(row)];
  return r[@"header"] ? nil : r;
}

- (void)toggleGrouped:(id)sender {
  [NSUserDefaults.standardUserDefaults setBool:!MachinesGrouped() forKey:kMachinesGroupedKey];
  [_machinesTable deselectAll:nil];
  [self reloadMachines];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
  return static_cast<NSInteger>(_rows.count);
}

- (BOOL)tableView:(NSTableView*)tableView isGroupRow:(NSInteger)row {
  return row >= 0 && static_cast<NSUInteger>(row) < _rows.count && _rows[static_cast<NSUInteger>(row)][@"header"] != nil;
}

- (CGFloat)tableView:(NSTableView*)tableView heightOfRow:(NSInteger)row {
  return [self tableView:tableView isGroupRow:row] ? 28 : 64;
}

- (BOOL)tableView:(NSTableView*)tableView shouldSelectRow:(NSInteger)row {
  return ![self tableView:tableView isGroupRow:row];
}

- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
  if ([self tableView:tableView isGroupRow:row]) {
    NSTableCellView* header = [tableView makeViewWithIdentifier:@"groupHeader" owner:self];
    if (!header) {
      header = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, tableView.bounds.size.width, 28)];
      header.identifier = @"groupHeader";
      NSTextField* t = [NSTextField labelWithString:@""];
      t.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
      t.textColor = NSColor.secondaryLabelColor;
      t.frame = NSMakeRect(14, 5, 400, 17);
      t.autoresizingMask = NSViewWidthSizable;
      header.textField = t;
      [header addSubview:t];
    }
    NSDictionary* h = _rows[static_cast<NSUInteger>(row)];
    header.textField.stringValue = [NSString stringWithFormat:@"%@  %@", h[@"header"], h[@"count"]];
    return header;
  }
  MachineCell* cell = [tableView makeViewWithIdentifier:@"machineCell" owner:self];
  if (!cell) {
    cell = [[MachineCell alloc] initWithFrame:NSMakeRect(0, 0, column.width, 64)];
    cell.identifier = @"machineCell";
  }
  [self configureCell:cell row:row];
  return cell;
}

// Fills a row's cell from the list and the live state.
- (void)configureCell:(MachineCell*)cell row:(NSInteger)row {
  NSDictionary* m = [self machineAtRow:row];
  if (!m) return;
  cell.action.target = self;
  cell.action.action = @selector(rowAction:);
  NSString* target = m[@"target"];
  cell.name.stringValue = MachineTitle(m);
  // An alias hides the server's own name: keep that one at hand.
  cell.name.toolTip = [m[@"alias"] length] && [m[@"name"] length] ? [@"服务端名称：" stringByAppendingString:m[@"name"]] : nil;
  NSString* user = [m[@"user"] length] ? m[@"user"] : @"–";
  RemoteWindowController* open = [self windowForTarget:target];
  if (open && open.isLive) {
    [cell setStatus:@"已连接" tint:NSColor.systemGreenColor];
    cell.detail.stringValue = [NSString stringWithFormat:@"%@ · %@ · %@", target, user, [open liveDetail]];
    cell.action.title = @"显示窗口";
  } else if (open) {
    const int64_t down = open.reconnectingSince ? static_cast<int64_t>(-open.reconnectingSince.timeIntervalSinceNow) : 0;
    [cell setStatus:@"正在重连…" tint:NSColor.systemOrangeColor];
    cell.detail.stringValue = [NSString stringWithFormat:@"%@ · %@ · 断开 %@，最多重试 1 分钟", target, user, DurationText(down)];
    cell.action.title = @"显示窗口";
  } else {
    NSNumber* up = _online[target];
    if (!up) [cell setStatus:@"" tint:nil];
    else if (up.boolValue) [cell setStatus:@"在线" tint:NSColor.systemBlueColor];
    else [cell setStatus:@"离线" tint:nil];
    NSString* when = @"";
    if ([m[@"last"] isKindOfClass:NSDate.class]) {
      NSRelativeDateTimeFormatter* f = [[NSRelativeDateTimeFormatter alloc] init];
      f.locale = [NSLocale localeWithLocaleIdentifier:@"zh_CN"];
      NSDate* last = m[@"last"];
      when = -last.timeIntervalSinceNow < 60
                 ? @"上次 刚刚"
                 : [@"上次 " stringByAppendingString:[f localizedStringForDate:last relativeToDate:NSDate.date]];
    }
    NSString* saved = HasSavedLogin(target) ? @"已保存密码" : @"需要密码";
    cell.detail.stringValue = [NSString stringWithFormat:@"%@ · %@%@%@ · %@", target, user, when.length ? @" · " : @"", when, saved];
    cell.action.title = @"连接";
  }
  cell.needsLayout = YES;
}

- (RemoteWindowController*)windowForTarget:(NSString*)target {
  const std::string key = MachineKey(target);
  for (RemoteWindowController* r in _remotes) {
    if (!key.empty() && MachineKey(r.target) == key) return r;
  }
  return nil;
}

// The window in front, if it is a remote one.
- (RemoteWindowController*)frontRemote {
  for (RemoteWindowController* r in _remotes) {
    if (r.window.isKeyWindow || r.window.isMainWindow) return r;
  }
  return _remotes.lastObject;
}

// The row a context-menu item or button acts on.
- (NSDictionary*)chosenMachine {
  NSInteger row = _machinesTable.clickedRow >= 0 ? _machinesTable.clickedRow : _machinesTable.selectedRow;
  return [self machineAtRow:row];
}

- (void)connectSelected:(id)sender {
  NSDictionary* m = [self chosenMachine];
  if (!m || [self connectingTo:m[@"target"]]) return;
  if (RemoteWindowController* open = [self windowForTarget:m[@"target"]]) {
    [_machinesWindow orderOut:nil];
    [open.window makeKeyAndOrderFront:nil];  // already connected: no second session to the same Mac
    return;
  }
  if ([self connectWithSavedLoginTo:m[@"target"] remote:nil]) return;
  // No saved password: the form, filled in, asking only for the password.
  _host.stringValue = m[@"target"];
  _user.stringValue = [m[@"user"] length] ? m[@"user"] : NSUserName();
  _password.stringValue = @"";
  [self showConnectWindow];
  [_connectWindow makeFirstResponder:_password];
}

- (void)forgetMachinePassword:(id)sender {
  NSDictionary* m = [self chosenMachine];
  if (!m) return;
  ForgetLogin(MachineKey(m[@"target"]));
  [_machinesTable reloadData];
}

- (void)removeMachine:(id)sender {
  NSDictionary* m = [self chosenMachine];
  if (!m) return;
  // Its password and its remembered fingerprint go too: next time it is a
  // new computer.
  const std::string key = MachineKey(m[@"target"]);
  ForgetLogin(key);
  std::string error;
  if (!key.empty()) _knownHosts->Forget(key, &error);
  NSMutableArray* list = [LoadMachines() mutableCopy];
  [list filterUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSDictionary* x, NSDictionary*) {
          return ![x[@"target"] isEqual:m[@"target"]];
        }]];
  SaveMachines(list);
  [self reloadMachines];
  if (_machines.count == 0) [self showMachines];  // the form
}

- (void)editMachine:(id)sender {
  NSDictionary* m = [self chosenMachine];
  if (!m) return;
  NSAlert* a = [[NSAlert alloc] init];
  a.messageText = [NSString stringWithFormat:@"编辑 %@", MachineTitle(m)];
  [a addButtonWithTitle:@"保存"];
  [a addButtonWithTitle:@"取消"];
  NSView* box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 128)];
  NSTextField* alias = [NSTextField textFieldWithString:m[@"alias"] ?: @""];
  alias.frame = NSMakeRect(80, 102, 220, 24);
  alias.placeholderString = [m[@"name"] length] ? m[@"name"] : @"列表里显示的名字";
  NSComboBox* group = [[NSComboBox alloc] initWithFrame:NSMakeRect(80, 68, 220, 26)];
  group.placeholderString = @"不分组";
  group.stringValue = m[@"group"] ?: @"";
  NSMutableOrderedSet* known = [NSMutableOrderedSet orderedSet];
  for (NSDictionary* x in LoadMachines()) {
    if ([x[@"group"] length]) [known addObject:x[@"group"]];
  }
  [group addItemsWithObjectValues:[known.array sortedArrayUsingSelector:@selector(localizedStandardCompare:)]];
  NSTextField* target = [NSTextField textFieldWithString:m[@"target"]];
  target.frame = NSMakeRect(80, 34, 220, 24);
  NSTextField* user = [NSTextField textFieldWithString:m[@"user"] ?: @""];
  user.frame = NSMakeRect(80, 2, 220, 24);
  [box addSubview:Label(@"别名", NSMakeRect(0, 104, 74, 20))];
  [box addSubview:alias];
  [box addSubview:Label(@"分组", NSMakeRect(0, 71, 74, 20))];
  [box addSubview:group];
  [box addSubview:Label(@"电脑", NSMakeRect(0, 36, 74, 20))];
  [box addSubview:target];
  [box addSubview:Label(@"账号", NSMakeRect(0, 4, 74, 20))];
  [box addSubview:user];
  a.accessoryView = box;
  a.window.initialFirstResponder = alias;
  if ([a runModal] != NSAlertFirstButtonReturn) return;
  NSString* newTarget = [target.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  NSString* newUser = [user.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  const std::string old_key = MachineKey(m[@"target"]), new_key = MachineKey(newTarget);
  if (new_key.empty()) return;
  // The entry as it is now: a connection made while the dialog was open may
  // have rewritten it (its time).
  NSMutableArray* list = [LoadMachines() mutableCopy];
  const NSUInteger i = [list indexOfObjectPassingTest:^BOOL(NSDictionary* x, NSUInteger, BOOL*) {
    return [x[@"target"] isEqual:m[@"target"]];
  }];
  if (i == NSNotFound) return;
  m = list[i];
  // The saved password follows the computer to its new address. It goes only
  // when the account was changed here to another one than it is for.
  std::string old_user, password;
  if (LoadLogin(old_key, &old_user, &password)) {
    const BOOL userEdited = ![newUser isEqualToString:m[@"user"] ?: @""];
    if (userEdited && ![newUser isEqualToString:NS(old_user)]) {
      ForgetLogin(old_key);
    } else if (new_key != old_key) {
      ForgetLogin(old_key);
      SaveLogin(new_key, old_user, password);
    }
    std::fill(password.begin(), password.end(), '\0');
  }
  NSMutableDictionary* edited = [m mutableCopy];
  edited[@"target"] = newTarget;
  edited[@"user"] = newUser;
  NSCharacterSet* ws = NSCharacterSet.whitespaceCharacterSet;
  NSString* newAlias = [alias.stringValue stringByTrimmingCharactersInSet:ws];
  NSString* newGroup = [group.stringValue stringByTrimmingCharactersInSet:ws];
  if (newAlias.length) edited[@"alias"] = newAlias;
  else [edited removeObjectForKey:@"alias"];
  if (newGroup.length) edited[@"group"] = newGroup;
  else [edited removeObjectForKey:@"group"];
  list[i] = edited;
  SaveMachines(list);
  [self reloadMachines];
}

// Connects to `target` with the login remembered for it, into `remote` (a
// reconnect) or a new window. NO if there is no remembered login. The form
// is not touched: the user may be filling it in for another computer.
- (BOOL)connectWithSavedLoginTo:(NSString*)target remote:(RemoteWindowController*)remote {
  std::string host;
  uint16_t port = 0;
  rdm::Auth auth;
  if (!target || !ParseTarget(target, &host, &port) ||
      !LoadLogin(rdm::KnownHosts::Key(host, port), &auth.username, &auth.password)) {
    return NO;
  }
  const BOOL started = [self startConnectTo:target auth:auth remote:remote automatic:YES];
  std::fill(auth.password.begin(), auth.password.end(), '\0');
  return started;
}

// After a drop: the same window, the same server, until it works or a
// minute has passed.
- (void)reconnect:(RemoteWindowController*)remote {
  // Closed meanwhile, or connected again already (an attempt that was under
  // way when "立即重试" was pressed).
  if (remote.closed || ![_remotes containsObject:remote] || remote.isLive) return;
  if ([self attemptForRemote:remote]) return;  // one is under way: when it fails, the next is scheduled
  if (![self connectWithSavedLoginTo:remote.target remote:remote]) {
    [remote.window close];
    [self alert:@"连接已断开" text:@"连接断了，而且没有保存的密码可以用来重新连接。"];
  }
}

- (ConnectAttempt*)attemptForRemote:(RemoteWindowController*)remote {
  for (ConnectAttempt* a in _attempts) {
    if (a.remote == remote && !a.cancelled) return a;
  }
  return nil;
}

// A connection to `target` is being made (not counting ones given up).
- (BOOL)connectingTo:(NSString*)target {
  const std::string key = MachineKey(target);
  for (ConnectAttempt* a in _attempts) {
    if (!a.cancelled && !key.empty() && a->key == key) return YES;
  }
  return NO;
}

// The form and the list are busy while a connection the user started is
// under way; a window reconnecting by itself does not block them.
- (void)updateBusy {
  BOOL busy = NO;
  for (ConnectAttempt* a in _attempts) busy = busy || (!a.reused && !a.cancelled);
  [self setBusy:busy];
}

- (void)connectToAnother:(id)sender {
  [self showMachines];  // the windows already open stay connected
}

// The app opened again (Dock icon, Finder) while it runs: the list, to pick
// another computer; the connected windows stay as they are.
- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)visible {
  if (!_machinesWindow.visible && !_connectWindow.visible) [self showMachines];
  return NO;
}

- (void)forgetSavedPassword:(id)sender {
  std::string host;
  uint16_t port = 0;
  NSString* target = [self frontRemote].target ?: [NSUserDefaults.standardUserDefaults stringForKey:@"lastHost"];
  if (!target || !ParseTarget(target, &host, &port)) return;
  ForgetLogin(rdm::KnownHosts::Key(host, port));
  [_machinesTable reloadData];
  NSAlert* a = [[NSAlert alloc] init];
  a.messageText = [NSString stringWithFormat:@"已忘记 %@ 的密码", target];
  a.informativeText = @"下次连接时会再问你。";
  [a runModal];
}

- (void)toggleStatsShown:(NSMenuItem*)item {
  [NSUserDefaults.standardUserDefaults setBool:!StatsShown() forKey:kStatsShownKey];
  [NSNotificationCenter.defaultCenter postNotificationName:kStatsChanged object:nil];
}

- (void)toggleStatsField:(NSMenuItem*)item {
  NSMutableSet* on = [StatsFieldsOn() mutableCopy];
  NSString* key = item.representedObject;
  if ([on containsObject:key]) {
    [on removeObject:key];
  } else {
    [on addObject:key];
  }
  [NSUserDefaults.standardUserDefaults setObject:on.allObjects forKey:kStatsFieldsKey];
  [NSNotificationCenter.defaultCenter postNotificationName:kStatsChanged object:nil];
}

- (void)setStatsOpacity:(NSMenuItem*)item {
  [NSUserDefaults.standardUserDefaults setInteger:item.tag forKey:kStatsOpacityKey];
  [NSNotificationCenter.defaultCenter postNotificationName:kStatsChanged object:nil];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
  if (item.action == @selector(toggleGrouped:)) {
    item.state = MachinesGrouped() ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
  }
  if (item.action == @selector(toggleStatsShown:)) {
    item.state = StatsShown() ? NSControlStateValueOn : NSControlStateValueOff;
  } else if (item.action == @selector(toggleStatsField:)) {
    item.state = [StatsFieldsOn() containsObject:item.representedObject] ? NSControlStateValueOn : NSControlStateValueOff;
    return StatsShown();
  } else if (item.action == @selector(setStatsOpacity:)) {
    item.state = item.tag == StatsOpacity() ? NSControlStateValueOn : NSControlStateValueOff;
    return StatsShown();
  } else if (item.action == @selector(setInputMode:)) {
    item.state = item.tag == static_cast<NSInteger>(CurrentInputMode()) ? NSControlStateValueOn : NSControlStateValueOff;
  } else if (item.action == @selector(toggleUseAllDisplays:)) {
    item.state = UseAllDisplays() ? NSControlStateValueOn : NSControlStateValueOff;
  } else if (item.action == @selector(togglePlaySound:)) {
    item.state = PlaySound() ? NSControlStateValueOn : NSControlStateValueOff;
  } else if (item.action == @selector(toggleObserveOnly:)) {
    item.state = gObserveOnly ? NSControlStateValueOn : NSControlStateValueOff;
  } else if (item.action == @selector(toggleTextMode:)) {
    item.state = TextMode() ? NSControlStateValueOn : NSControlStateValueOff;
  } else if (item.action == @selector(toggleTextModeStandardResolution:)) {
    item.state = TextModeStandardResolution() ? NSControlStateValueOn : NSControlStateValueOff;
    return TextMode();
  } else if (item.action == @selector(setUpscaler:)) {
    item.state = item.tag == static_cast<NSInteger>(CurrentUpscaler()) ? NSControlStateValueOn : NSControlStateValueOff;
  }
  return YES;
}

- (void)toggleTextMode:(NSMenuItem*)item {
  const bool on = !TextMode();
  [NSUserDefaults.standardUserDefaults setBool:on forKey:kTextModeKey];
  item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
  [NSNotificationCenter.defaultCenter postNotificationName:kTextModeChanged object:nil];
}

- (void)toggleObserveOnly:(id)sender {
  gObserveOnly = !gObserveOnly;
  [NSNotificationCenter.defaultCenter postNotificationName:kObserveChanged object:nil];
}

- (void)togglePlaySound:(NSMenuItem*)item {
  const bool on = !PlaySound();
  [NSUserDefaults.standardUserDefaults setBool:on forKey:kPlaySoundKey];
  item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
  [NSNotificationCenter.defaultCenter postNotificationName:kPlaySoundChanged object:nil];
}

- (void)toggleTextModeStandardResolution:(NSMenuItem*)item {
  const bool on = !TextModeStandardResolution();
  [NSUserDefaults.standardUserDefaults setBool:on forKey:kTextModeStandardResolutionKey];
  item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
  [NSNotificationCenter.defaultCenter postNotificationName:kTextModeChanged object:nil];
}

- (void)setUpscaler:(NSMenuItem*)item {
  SetUpscaler(static_cast<Upscaler>(item.tag));
  [NSNotificationCenter.defaultCenter postNotificationName:kUpscalerChanged object:nil];
}

- (void)toggleUseAllDisplays:(NSMenuItem*)item {
  const bool on = !UseAllDisplays();
  [NSUserDefaults.standardUserDefaults setBool:on forKey:kUseAllDisplaysKey];
  item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
  [NSNotificationCenter.defaultCenter postNotificationName:kUseAllDisplaysChanged object:nil];
}

- (void)setInputMode:(NSMenuItem*)item {
  SetInputMode(static_cast<InputMode>(item.tag));
  // Each window leaves or enters the input method mode first, then the sync
  // follows (or stops).
  [NSNotificationCenter.defaultCenter postNotificationName:kUseLocalInputMethodChanged object:nil];
  [NSNotificationCenter.defaultCenter postNotificationName:kSyncKeyboardLanguageChanged object:nil];
}

// ---- the menu bar icon ----------------------------------------------------

- (void)buildStatusItem {
  _ringStep = -1;
  _statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
  _statusItem.button.toolTip = @"RDMachine";
  _statusMenu = [[NSMenu alloc] init];
  _statusMenu.delegate = self;
  _statusMenu.autoenablesItems = NO;
  _statusItem.menu = _statusMenu;
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(fileReceiveChanged:)
                                             name:@(rdm::kFileReceiveChangedNotification)
                                           object:nil];
  [self updateStatusItem];
  // Common modes: it keeps up while the menu is open.
  _statusTimer = [NSTimer timerWithTimeInterval:2.0 target:self selector:@selector(updateStatusItem) userInfo:nil repeats:YES];
  [NSRunLoop.mainRunLoop addTimer:_statusTimer forMode:NSRunLoopCommonModes];
}

- (void)fileReceiveChanged:(NSNotification*)note {
  const rdm::FileReceiveProgress p = rdm::CurrentFileReceive();
  int step = -1;
  if (p.active) step = p.total > 0 ? static_cast<int>(40.0 * std::min<uint64_t>(p.bytes, p.total) / p.total) : 41;
  if (step == _ringStep) return;  // only when it changes: a new image relays out the item
  _ringStep = step;
  if (step >= 0) {
    _statusSymbol = nil;
    _statusItem.button.image = RDMFileProgressRing(step > 40 ? -1 : step / 40.0);
    _statusItem.button.toolTip = @(rdm::DescribeFileReceive(p).c_str());
  }
  [self updateStatusItem];
}

// The icon (unless the ring is up) and the menu, as things are now.
- (void)updateStatusItem {
  if (_ringStep < 0) {
    NSUInteger live = 0;
    for (RemoteWindowController* r in _remotes) live += r.isLive ? 1 : 0;
    NSString* symbol = live > 0 ? @"macwindow.on.rectangle" : @"macwindow";
    if (![symbol isEqualToString:_statusSymbol]) {
      _statusSymbol = symbol;
      NSImage* img = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:@"RDMachine"];
      [img setTemplate:YES];
      _statusItem.button.image = img;
      _statusItem.button.title = img ? @"" : @"RD";
    }
    _statusItem.button.toolTip = live == 0   ? @"RDMachine：没有连接"
                                 : live == 1 ? @"RDMachine：已连接 1 台电脑"
                                             : [NSString stringWithFormat:@"RDMachine：已连接 %lu 台电脑", (unsigned long)live];
  } else {
    _statusItem.button.toolTip = @(rdm::DescribeFileReceive(rdm::CurrentFileReceive()).c_str());
  }
  // Keep the menu filled at all times: a status item whose menu is empty
  // does not open on recent macOS (menus hosted out of process).
  if (!_statusMenuOpen) [self rebuildStatusMenu];
}

- (void)rebuildStatusMenu {
  [_statusMenu removeAllItems];
  auto add = [&](NSString* title, SEL action) {
    NSMenuItem* item = [_statusMenu addItemWithTitle:title action:action keyEquivalent:@""];
    item.target = self;
    item.enabled = action != nullptr;
    return item;
  };
  const rdm::FileReceiveProgress p = rdm::CurrentFileReceive();
  if (p.active) {
    add(@(rdm::DescribeFileReceive(p).c_str()), nullptr);
    [_statusMenu addItem:NSMenuItem.separatorItem];
  }
  if (_remotes.count == 0) add(@"没有连接", nullptr);
  for (RemoteWindowController* r in _remotes) {
    NSString* name = r.serverName ?: r.target ?: @"?";
    NSString* state = r.isLive ? [r liveDetail] : r.reconnectingSince ? @"正在重新连接…" : @"未连接";
    NSMenuItem* item = add([NSString stringWithFormat:@"%@ — %@", name, state], @selector(showRemoteFromStatus:));
    item.representedObject = r;
  }
  [_statusMenu addItem:NSMenuItem.separatorItem];
  add(@"电脑列表…", @selector(showMachinesFromStatus:));
  add(@"新建连接…", @selector(newConnection:));
  [_statusMenu addItem:NSMenuItem.separatorItem];
  NSMenuItem* quit = [_statusMenu addItemWithTitle:@"退出 rdmachine" action:@selector(terminate:) keyEquivalent:@""];
  quit.target = NSApp;
}

- (void)showRemoteFromStatus:(NSMenuItem*)item {
  RemoteWindowController* r = item.representedObject;
  if (![_remotes containsObject:r]) return;
  [NSApp activateIgnoringOtherApps:YES];
  [r.window makeKeyAndOrderFront:nil];
}

- (void)showMachinesFromStatus:(id)sender {
  [NSApp activateIgnoringOtherApps:YES];
  [self showMachines];
}

- (void)menuWillOpen:(NSMenu*)menu {
  if (menu == _statusMenu) _statusMenuOpen = YES;
}

- (void)menuDidClose:(NSMenu*)menu {
  if (menu == _statusMenu) _statusMenuOpen = NO;
}

- (void)buildMenu {
  NSMenu* bar = [[NSMenu alloc] init];
  NSMenuItem* appItem = [[NSMenuItem alloc] init];
  [bar addItem:appItem];
  NSMenu* appMenu = [[NSMenu alloc] init];
  [appMenu addItemWithTitle:@"连接到其他电脑…" action:@selector(connectToAnother:) keyEquivalent:@"n"]
      .target = self;
  [appMenu addItemWithTitle:@"忘记已保存的密码" action:@selector(forgetSavedPassword:) keyEquivalent:@""]
      .target = self;
  [appMenu addItem:NSMenuItem.separatorItem];
  [appMenu addItemWithTitle:@"退出 rdmachine" action:@selector(terminate:) keyEquivalent:@"q"];
  appItem.submenu = appMenu;
  NSMenuItem* editItem = [[NSMenuItem alloc] init];
  [bar addItem:editItem];
  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"编辑"];
  [edit addItemWithTitle:@"剪切" action:@selector(cut:) keyEquivalent:@"x"];
  [edit addItemWithTitle:@"拷贝" action:@selector(copy:) keyEquivalent:@"c"];
  [edit addItemWithTitle:@"粘贴" action:@selector(paste:) keyEquivalent:@"v"];
  [edit addItemWithTitle:@"全选" action:@selector(selectAll:) keyEquivalent:@"a"];
  editItem.submenu = edit;
  NSMenuItem* viewItem = [[NSMenuItem alloc] init];
  [bar addItem:viewItem];
  NSMenu* view = [[NSMenu alloc] initWithTitle:@"显示"];
  // In a remote window the key goes through localShortcut: instead (every
  // key there goes to the server); the menu item is for finding it.
  NSMenuItem* fs = [view addItemWithTitle:@"进入/退出全屏" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
  fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagCommand;
  NSMenuItem* all = [view addItemWithTitle:@"全屏时使用所有显示器" action:@selector(toggleUseAllDisplays:) keyEquivalent:@""];
  all.target = self;
  all.state = UseAllDisplays() ? NSControlStateValueOn : NSControlStateValueOff;
  all.toolTip = @"全屏时，这台 Mac 的每块屏幕都对应那台 Mac 上的一块显示器（扩展桌面），按这里屏幕的排列放好。";
  NSMenuItem* observe = [view addItemWithTitle:@"只观看（不控制）" action:@selector(toggleObserveOnly:) keyEquivalent:@"o"];
  observe.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagCommand;
  observe.target = self;
  observe.toolTip = @"只看那台 Mac 的画面：这里的键盘、鼠标和滚动都不发过去。再选一次（或按 ⌃⌥⌘O）恢复控制。每次打开客户端都从控制开始。";
  [view addItem:NSMenuItem.separatorItem];
  NSMenuItem* textMode = [view addItemWithTitle:@"省流量（文字清晰）"
                                         action:@selector(toggleTextMode:)
                                  keyEquivalent:@""];
  textMode.target = self;
  textMode.state = TextMode() ? NSControlStateValueOn : NSControlStateValueOff;
  textMode.toolTip = @"画面和平时一样，流量更少：静止和正在输入的文字用更紧凑的方式发送（颜色略少）；滚动和视频照常，省得少些。";
  NSMenuItem* textRes = [view addItemWithTitle:@"省流量时用标准分辨率（1 倍）"
                                        action:@selector(toggleTextModeStandardResolution:)
                                 keyEquivalent:@""];
  textRes.target = self;
  textRes.indentationLevel = 1;
  textRes.state = TextModeStandardResolution() ? NSControlStateValueOn : NSControlStateValueOff;
  textRes.toolTip = @"开着省流量时，用 1 倍的桌面代替 Retina（2 倍）：像素只有四分之一，流量少很多；字会粗糙一些。";
  // How that 1x desktop is enlarged on a Retina screen (docs/upscaler.md).
  NSMenuItem* upItem = [view addItemWithTitle:@"1 倍画面放大" action:nil keyEquivalent:@""];
  upItem.indentationLevel = 1;
  NSMenu* up = [[NSMenu alloc] initWithTitle:@"1 倍画面放大"];
  const struct {
    Upscaler how;
    NSString* title;
    NSString* tip;
  } ups[] = {
      {Upscaler::kModel, @"文字模型",
       @"用一个按 macOS 文字训练的小模型放大，字的边缘最清楚（笔画特别多的字补不回来）。画面大面积变化时先用 MetalFX，停下后再用模型。"},
      {Upscaler::kMetalFX, @"MetalFX", @"用系统自带的 MetalFX 空间超分放大：没有方块，边缘比平滑放大锐。"},
      {Upscaler::kPixels, @"像素重复", @"每个像素原样放大成 2 × 2：和服务端画的一模一样，但有方块感。"},
  };
  for (const auto& u : ups) {
    NSMenuItem* item = [up addItemWithTitle:u.title action:@selector(setUpscaler:) keyEquivalent:@""];
    item.target = self;
    item.tag = static_cast<NSInteger>(u.how);
    item.state = CurrentUpscaler() == u.how ? NSControlStateValueOn : NSControlStateValueOff;
    item.toolTip = u.tip;
  }
  upItem.submenu = up;
  NSMenuItem* sound = [view addItemWithTitle:@"播放远程电脑的声音" action:@selector(togglePlaySound:) keyEquivalent:@""];
  sound.target = self;
  sound.state = PlaySound() ? NSControlStateValueOn : NSControlStateValueOff;
  sound.toolTip = @"那台 Mac 放的声音在这里播放（那台 Mac 自己默认不出声）。关掉后这次连接不再传声音，以后的连接也不要声音。";
  [view addItem:NSMenuItem.separatorItem];
  [view addItemWithTitle:@"按分组显示电脑" action:@selector(toggleGrouped:) keyEquivalent:@""].target = self;
  [view addItem:NSMenuItem.separatorItem];
  NSMenuItem* statsItem = [view addItemWithTitle:@"统计信息" action:nil keyEquivalent:@""];
  NSMenu* stats = [[NSMenu alloc] initWithTitle:@"统计信息"];
  [stats addItemWithTitle:@"显示统计信息" action:@selector(toggleStatsShown:) keyEquivalent:@""].target = self;
  [stats addItem:NSMenuItem.separatorItem];
  for (const auto& f : kStatsFields) {
    NSMenuItem* it = [stats addItemWithTitle:f.title action:@selector(toggleStatsField:) keyEquivalent:@""];
    it.target = self;
    it.representedObject = f.key;
    it.indentationLevel = 1;
  }
  [stats addItem:NSMenuItem.separatorItem];
  [stats addItemWithTitle:@"背景透明度" action:nil keyEquivalent:@""].enabled = NO;
  for (int pct : kStatsOpacities) {
    NSMenuItem* it = [stats addItemWithTitle:[NSString stringWithFormat:@"%d%%", pct]
                                      action:@selector(setStatsOpacity:)
                               keyEquivalent:@""];
    it.target = self;
    it.tag = pct;
    it.indentationLevel = 1;
  }
  statsItem.submenu = stats;
  viewItem.submenu = view;
  // 输入法: a menu of its own, one choice of three.
  NSMenuItem* imeItem = [[NSMenuItem alloc] init];
  [bar addItem:imeItem];
  NSMenu* ime = [[NSMenu alloc] initWithTitle:@"输入法"];
  // Typing: one of three (docs/ime.md). The server follows: RDMachine Input
  // for this Mac's input method, the matching input source for sync.
  struct InputItem {
    NSString* title;
    NSString* tip;
    InputMode mode;
  };
  const InputItem inputs[] = {
      {@"用这台 Mac 的输入法",
       @"用这台 Mac 的输入法打字，候选窗在这里画、贴着那台 Mac 的光标，不用等网络；那台 Mac 自动切到 RDMachine Input。"
       @"需要被控端 6.2 以上，并在它的系统设置里加上 RDMachine Input；不满足时按“同步键盘语言”。",
       InputMode::kLocal},
      {@"同步键盘语言", @"被控端跟着这台 Mac 切换输入法（拼音、ABC 等），包括用 Caps Lock 切换的时候；组字和候选窗在被控端。",
       InputMode::kSync},
      {@"按键原样发", @"不同步，也不用这台 Mac 的输入法：被控端用它自己选的输入法。", InputMode::kOff},
  };
  for (const InputItem& in : inputs) {
    NSMenuItem* item = [ime addItemWithTitle:in.title action:@selector(setInputMode:) keyEquivalent:@""];
    item.target = self;
    item.tag = static_cast<NSInteger>(in.mode);
    item.state = CurrentInputMode() == in.mode ? NSControlStateValueOn : NSControlStateValueOff;
    item.toolTip = in.tip;
  }
  imeItem.submenu = ime;
  NSApp.mainMenu = bar;
}

- (void)setBusy:(BOOL)busy {
  _connectButton.enabled = !busy;
  _host.enabled = !busy;
  _machinesConnect.enabled = !busy;
  if (busy) {
    [_spinner startAnimation:nil];
    [_machinesSpinner startAnimation:nil];
  } else {
    [_spinner stopAnimation:nil];
    [_machinesSpinner stopAnimation:nil];
  }
}

- (void)alert:(NSString*)title text:(NSString*)text {
  [self alert:title text:text target:nil];
}

- (void)alert:(NSString*)title text:(NSString*)text target:(NSString*)target {
  if (!_connectWindow.visible) {
    [self showMachines];
    if (_machinesWindow.visible) {
      [self showNotice:title detail:[text stringByReplacingOccurrencesOfString:@"\n\n" withString:@" "]
                target:target
               warning:NO];
      return;
    }
  }
  NSAlert* a = [[NSAlert alloc] init];
  a.messageText = title;
  a.informativeText = text;
  NSWindow* w = _connectWindow.visible ? _connectWindow : _machinesWindow;
  if (!w.visible) [self showMachines];
  w = _connectWindow.visible ? _connectWindow : _machinesWindow;
  [a beginSheetModalForWindow:w completionHandler:nil];
}

// Background thread: asks on the main thread whether to trust a first-time
// fingerprint.
- (BOOL)askTrust:(NSString*)host fingerprint:(NSString*)fp {
  __block BOOL yes = NO;
  dispatch_sync(dispatch_get_main_queue(), ^{
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"第一次连接 %@", host];
    a.informativeText = [NSString
        stringWithFormat:@"请核对这个指纹和那台 Mac 显示的一致（菜单栏里的“复制本机指纹”）：\n\n%@\n\n确认后会记住，以后指纹变了会提醒你。",
                         fp];
    [a addButtonWithTitle:@"信任并连接"];
    [a addButtonWithTitle:@"取消"];
    yes = [a runModal] == NSAlertFirstButtonReturn;
  });
  return yes;
}

// Background thread: the server's identity changed (reinstalled, say). The
// user decides; rdmachine trusts whoever uses this Mac.
- (BOOL)askTrustChanged:(NSString*)host old:(NSString*)oldFp now:(NSString*)newFp {
  __block BOOL yes = NO;
  dispatch_sync(dispatch_get_main_queue(), ^{
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"%@ 的身份变了", host];
    a.informativeText = [NSString
        stringWithFormat:@"它的指纹和记住的不一样。重装过系统或者重新生成过身份时会这样；如果你没有预料到，就不要连接。\n\n记住的：\n%@\n\n现在的：\n%@",
                         oldFp, newFp];
    [a addButtonWithTitle:@"信任新身份并连接"];
    [a addButtonWithTitle:@"取消"];
    yes = [a runModal] == NSAlertFirstButtonReturn;
  });
  return yes;
}

// The form's Connect button.
- (void)connect:(id)sender {
  NSString* target = [_host.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  rdm::Auth auth;
  auth.username = _user.stringValue.UTF8String;
  auth.password = _password.stringValue.UTF8String;
  if (![self startConnectTo:target auth:auth remote:nil automatic:NO]) return;
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  [defaults setObject:target forKey:@"lastHost"];
  [defaults setObject:_user.stringValue forKey:@"lastUser"];
  _password.stringValue = @"";
  std::fill(auth.password.begin(), auth.password.end(), '\0');
}

// Starts a connection to `target` in the background; connectFinished: gets
// the outcome. Into `reuse` (a window connecting again) or a new window. NO
// when nothing was started: no usable address, or the form's own connection
// is still under way.
- (BOOL)startConnectTo:(NSString*)target
                  auth:(const rdm::Auth&)auth
                remote:(RemoteWindowController*)reuse
             automatic:(BOOL)automatic {
  std::string host;
  uint16_t port = 0;
  if (!target || !ParseTarget(target, &host, &port)) return NO;
  if (!reuse) {
    // One session per computer: a second one would take the first one over.
    if (RemoteWindowController* open = [self windowForTarget:target]) {
      [open.window makeKeyAndOrderFront:nil];
      return YES;
    }
    if ([self connectingTo:target]) return YES;
  }

  rdm::RemoteSession::Options opts;
  opts.control.host = host;
  opts.control.port = port;
  opts.control.client_name = NSHost.currentHost.localizedName.UTF8String ?: "Mac";
  opts.control.auth = auth;
  ConnectAttempt* attempt = [[ConnectAttempt alloc] init];
  attempt->key = rdm::KnownHosts::Key(host, port);
  attempt->user = auth.username;
  // Remembered once it works (connectFinished).
  attempt->password = auth.password;
  attempt.target = target;
  attempt.reused = reuse != nil;
  attempt.automatic = automatic;

  auto changed = std::make_shared<bool>(false);
  __weak AppDelegate* weakSelf = self;
  NSString* hostName = NS(host);
  opts.control.trust_server = rdm::TrustWithKnownHosts(
      _knownHosts.get(), rdm::KnownHosts::Key(host, port),
      [weakSelf, hostName](const std::string& fp) {
        AppDelegate* me = weakSelf;
        return me && [me askTrust:hostName fingerprint:NS(rdm::FormatFingerprint(fp))];
      },
      changed,
      [weakSelf, hostName](const std::string& old_fp, const std::string& new_fp) {
        AppDelegate* me = weakSelf;
        return me && [me askTrustChanged:hostName
                                     old:NS(rdm::FormatFingerprint(old_fp))
                                     now:NS(rdm::FormatFingerprint(new_fp))];
      });

  // The window exists before the session starts: pictures may arrive while
  // Start() is still waiting for the connection to settle.
  // A reconnect goes into the window that is already there.
  RemoteWindowController* remote = reuse ?: [[RemoteWindowController alloc] initWithTitle:hostName];
  attempt.remote = remote;
  // Ask for a desktop made for this window: its pixels, its HiDPI scale. A
  // server that cannot build one shares its display as it is.
  opts.control.request.desktop = reuse ? [remote viewDesktop] : [remote initialDesktop];
  opts.control.request.text_mode = TextMode();
  // The server's sound, played on this Mac's default output (docs/audio.md).
  opts.audio = PlaySound();
  // Full screen with more than one screen here: an extended desktop, a
  // server display for each screen.
  const BOOL fullScreen = reuse ? [remote wantsFullScreen] : [NSUserDefaults.standardUserDefaults boolForKey:kRemoteFullScreenKey];
  if (fullScreen && UseAllDisplays()) opts.control.request.extra_desktops = ExtraDesktopsFor([remote homeScreen]);
  remote.extrasRequested = !opts.control.request.extra_desktops.empty();
  rda_viewer_config_default(&opts.viewer);
  opts.viewer.user = (__bridge void*)remote;
  opts.viewer.on_stream_info = &OnStreamInfo;
  opts.viewer.on_frame = &OnFrame;
  opts.viewer.on_cursor = &OnCursor;
  opts.viewer.on_message = &OnMessage;
  // Text, rich text, images, files and folders copied on either Mac can be
  // pasted on the other; they only cross when they are pasted (both ways
  // for now; a setting comes with the preferences window).
  opts.clipboard = SharedClipboard();
  // A paste cut short by a dropped connection goes on when this server is
  // back (the reconnect makes a new session).
  opts.file_target = rdm::SystemFileTarget(rdm::KnownHosts::Key(host, port));
  opts.file_staging_dir = rdm::FileStagingDir();
  opts.log = [](const std::string& line) { NSLog(@"%@", [NSString stringWithUTF8String:line.c_str()]); };
  __weak RemoteWindowController* weakRemote = remote;
  opts.on_moving = [weakRemote](const std::string& reason) {
    NSString* title = reason == "user_logged_in"    ? @"正在切换到桌面"
                      : reason == "user_logged_out" ? @"正在切换到登录窗口"
                      : reason == "user_switched"   ? @"正在切换用户"
                      : reason == "agent_restarted" ? @"服务重启了"
                      : reason == "server_stopping" ? @"那台 Mac 的服务停了"
                                                    : @"正在切换桌面";
    NSString* detail = reason == "user_logged_in"    ? @"那台 Mac 已经登录，画面马上换成登录后的桌面。"
                       : reason == "user_logged_out" ? @"那台 Mac 退出了登录，画面马上换成登录窗口。"
                       : reason == "user_switched"   ? @"那台 Mac 换成了另一个用户的桌面。"
                       : reason == "agent_restarted" ? @"那台 Mac 上的服务重新启动了，正在接上。"
                       : reason == "server_stopping" ? @"有人退出或重启了那台 Mac 上的服务，正在等它回来。"
                                                     : @"那台 Mac 的桌面变了，正在接上。";
    NSString* symbol = reason == "server_stopping" ? @"pause.circle" : @"arrow.triangle.2.circlepath";
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakRemote showStatus:title detail:detail symbol:symbol until:[NSDate dateWithTimeIntervalSinceNow:60] canRetry:NO];
    });
  };
  opts.on_stalled = [weakRemote](bool stalled) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (stalled) {
        [weakRemote showStatus:@"连接中断"
                        detail:@"几秒钟没收到那台 Mac 的消息：网络可能断了，或者那台 Mac 睡眠、关机了。"
                        symbol:@"wifi.exclamationmark"
                         until:[NSDate dateWithTimeIntervalSinceNow:8]
                      canRetry:NO];
      } else {
        [weakRemote hideStatus];
      }
    });
  };
  opts.on_moved = [weakRemote](const rdm::SessionReady& ready) {
    rdm::SessionReady copy = ready;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakRemote sessionMoved:copy];
    });
  };
  opts.on_ended = [weakSelf, weakRemote](const std::string& reason) {
    NSString* why = NS(reason);
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf sessionEnded:weakRemote reason:why];
    });
  };

  [_attempts addObject:attempt];
  [self updateBusy];
  // Blocks copy what they capture, so the move-only pieces travel in
  // shared holders. The background block holds the attempt (and through it
  // the window) only weakly: _attempts keeps it, so its last release, and
  // the window's, happen on the main thread.
  auto boxed = std::make_shared<rdm::RemoteSession::Options>(std::move(opts));
  auto holder = std::make_shared<std::unique_ptr<rdm::RemoteSession>>();
  auto result = std::make_shared<rdm::ConnectResult>();
  __weak ConnectAttempt* weakAttempt = attempt;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    *holder = rdm::RemoteSession::Start(std::move(*boxed), result.get());
    dispatch_async(dispatch_get_main_queue(), ^{
      AppDelegate* me = weakSelf;
      ConnectAttempt* a = weakAttempt;
      if (me && a) {
        [me connectFinished:a session:std::move(*holder) result:*result changed:*changed];
      } else {
        holder->reset();  // on the main thread, like every other session's end
      }
    });
  });
  return YES;
}

- (void)connectFinished:(ConnectAttempt*)attempt
                session:(std::unique_ptr<rdm::RemoteSession>)session
                 result:(const rdm::ConnectResult&)result
                changed:(bool)changed {
  [_attempts removeObject:attempt];
  [self updateBusy];
  RemoteWindowController* remote = attempt.remote;
  const BOOL reused = attempt.reused;
  // Given up (Cancel on the form): nothing is shown or saved; a session that
  // did start ends here.
  if (attempt.cancelled) return;
  const BOOL windowGone = reused && (remote.closed || ![_remotes containsObject:remote]);
  if (!session) {
    if (reused) {
      if (windowGone) return;  // the user closed it while it was trying: nothing to report
      // Reconnecting: keep trying while the window is open (minimized or
      // behind others counts) and the minute has not passed.
      const std::string& e = result.error;
      const bool transient = e == "connect_failed" || e == "connection_closed" || e == "timeout" ||
                             e == "media_failed" || e == "unavailable" || e == "busy" || e == "internal";
      if (transient && remote.reconnectUntil.timeIntervalSinceNow > 0) {
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(reconnect:) object:remote];
        [self performSelector:@selector(reconnect:) withObject:remote afterDelay:3.0];
        return;
      }
      [remote.window close];  // brings back the list
      [self alert:[NSString stringWithFormat:@"没能重新连上 %@", remote.serverName ?: remote.target]
             text:DescribeError(result)
           target:remote.target];
      return;
    }
    if (attempt.automatic && result.error == "bad_credentials") {
      // The password changed on that Mac: ask for the new one once.
      ForgetLogin(attempt->key);
      _host.stringValue = attempt.target ?: @"";
      _user.stringValue = NS(attempt->user);
      _password.stringValue = @"";
      [self showConnectWindow];
      [_connectWindow makeFirstResponder:_password];
      [self alert:@"保存的密码不能用了"
             text:@"可能在那台 Mac 上改过密码。输入新密码后会重新记住。"];
      return;
    }
    if (!_connectWindow.visible) [self showMachines];
    if (changed) {
      [self alert:@"那台电脑的身份变了"
             text:[NSString stringWithFormat:@"指纹和记住的不一样，所以没有连接。如果你知道原因（比如重装过），"
                                             @"可以在列表里删掉这台电脑再重新连接，或者删掉这个文件里它的那一行：\n\n%@",
                                             NS(_knownHosts->path())]];
    } else if (result.error != "untrusted_server") {
      [self alert:@"连接失败" text:DescribeError(result)];
    }
    return;
  }
  if (windowGone) return;  // closed meanwhile; the session closes here
  // Connected: nothing may connect into this window again by itself (a
  // retry that was waiting would take this very session over).
  [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(reconnect:) object:remote];
  if (!reused) [_remotes addObject:remote];
  if (!attempt->password.empty()) SaveLogin(attempt->key, attempt->user, attempt->password);
  remote.target = attempt.target;
  remote.serverName = NS(result.server.server_name);
  RememberMachine(remote.target, NS(attempt->user), remote.serverName);
  // A reconnect leaves the windows as they are: the user may be typing in
  // another one, or filling in the form.
  [remote attachSession:std::move(session) activate:!reused];
  remote.reconnectingSince = nil;
  remote.reconnectUntil = nil;
  remote.window.title = [NSString stringWithFormat:@"%@（%@）", remote.serverName, remote.target];
  if (!reused && [NSUserDefaults.standardUserDefaults boolForKey:kRemoteFullScreenKey]) {
    [remote enterFullScreen];  // as it was left last time
  }
  __weak AppDelegate* weakSelf = self;
  __weak RemoteWindowController* weakClosing = remote;
  remote.onDisplaysChanged = ^{
    AppDelegate* me = weakSelf;
    RemoteWindowController* r = weakClosing;
    if (me && r) [me switchDisplays:r];
  };
  remote.onClose = ^{
    // Called from the controller's own windowWillClose: release it on the
    // next turn of the run loop, not while it is still running.
    dispatch_async(dispatch_get_main_queue(), ^{
      AppDelegate* me = weakSelf;
      if (!me) return;
      if (weakClosing) [me->_remotes removeObject:weakClosing];
      if (me->_remotes.count == 0) [me showMachines];  // the last one closed: back to the list
    });
  };
  if (!reused) {
    // The password field must not stay the hidden window's first responder:
    // it is what turns secure input on.
    [_connectWindow makeFirstResponder:nil];
    [_connectWindow orderOut:nil];
    [_machinesWindow orderOut:nil];
  }
  ReleaseSecureInput();
}

// Full screen entered or left (or View > Use All Displays changed): the
// server builds the displays at the start of a session, so connect again
// with the new layout (the remembered login; without one, stay as it is).
- (void)switchDisplays:(RemoteWindowController*)remote {
  if (!remote.isLive || [self attemptForRemote:remote] || !HasSavedLogin(remote.target)) return;
  [remote endSession];
  [remote showStatus:@"正在切换显示器"
              detail:@"全屏时每块屏幕对应那台 Mac 上的一块显示器，正在按现在的屏幕重新连接。"
              symbol:@"rectangle.on.rectangle"
               until:[NSDate dateWithTimeIntervalSinceNow:60]
            canRetry:NO];
  remote.reconnectingSince = NSDate.date;
  remote.reconnectUntil = [NSDate dateWithTimeIntervalSinceNow:60];
  [self reconnect:remote];
}

- (void)sessionEnded:(RemoteWindowController*)remote reason:(NSString*)reason {
  if (!remote || ![_remotes containsObject:remote]) return;
  // The network or the server went away: connect again by itself, into the
  // same window, with the remembered login. Not when someone took over
  // (another client of this user's) or someone else logged in there.
  const BOOL lost = [reason isEqualToString:@"connection_lost"] || [reason isEqualToString:@"media_lost"] ||
                    [reason isEqualToString:@"server_shutdown"] || [reason isEqualToString:@"server_closed"] ||
                    [reason isEqualToString:@"idle_timeout"];
  std::string saved_user, saved_password;
  const std::string key = MachineKey(remote.target);
  if (lost && !key.empty() && LoadLogin(key, &saved_user, &saved_password)) {
    std::fill(saved_password.begin(), saved_password.end(), '\0');
    [remote endSession];
    remote.reconnectingSince = NSDate.date;
    remote.window.title = [NSString stringWithFormat:@"%@（%@）— 正在重连…", remote.serverName, remote.target];
    const BOOL stopped = [reason isEqualToString:@"server_shutdown"];
    [remote showStatus:@"正在重新连接"
                detail:stopped ? @"那台 Mac 的服务停了，它一回来就会自动连上。"
                               : @"和那台 Mac 的连接断了，正在自动重新连接。"
                symbol:stopped ? @"pause.circle" : @"arrow.clockwise"
                 until:[NSDate dateWithTimeIntervalSinceNow:60]
              canRetry:YES];
    __weak AppDelegate* weakSelf = self;
    __weak RemoteWindowController* weakRetry = remote;
    remote.onRetry = ^{
      AppDelegate* me = weakSelf;
      RemoteWindowController* r = weakRetry;
      if (!me || !r) return;
      [NSObject cancelPreviousPerformRequestsWithTarget:me selector:@selector(reconnect:) object:r];
      [me reconnect:r];
    };
    remote.reconnectUntil = [NSDate dateWithTimeIntervalSinceNow:60];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(reconnect:) object:remote];
    [self performSelector:@selector(reconnect:) withObject:remote afterDelay:1.0];
    return;
  }
  NSString* who = remote.serverName ?: remote.target;
  NSString* where = remote.target;
  [remote.window close];  // ends the session and brings back the list
  NSString* text = [reason isEqualToString:@"server_disconnected"]
                       ? @"那台 Mac 上有人在菜单里点了“断开当前连接”。"
                   : [reason isEqualToString:@"session_replaced"]
                       ? @"你从另一台电脑连上了那台 Mac，会话已经转到那边。"
                   : [reason isEqualToString:@"console_changed"]
                       ? @"那台 Mac 上换了别人登录，会话已结束。"
                   : [reason isEqualToString:@"server_shutdown"] ? @"那台 Mac 的服务停止了。"
                   : [reason isEqualToString:@"media_lost"]    ? @"画面连接断了。"
                   : [reason isEqualToString:@"connection_lost"]
                       ? @"和那台电脑的连接断了。"
                       : [NSString stringWithFormat:@"会话结束了（%@）。", reason];
  // Someone took over or someone else logged in: reconnecting would undo
  // that, so no button for it.
  const BOOL offer = ![reason isEqualToString:@"session_replaced"] && ![reason isEqualToString:@"console_changed"];
  [self alert:[NSString stringWithFormat:@"与 %@ 的连接已断开", who] text:text target:offer ? where : nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  return _remotes.count == 0;
}

- (void)applicationWillTerminate:(NSNotification*)note {
  for (RemoteWindowController* r in _remotes) {
    [r saveWindowMode];  // quitting with the window open counts too
    [r endSession];
  }
}

@end
