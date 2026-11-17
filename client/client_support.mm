#include "client_support.h"

namespace rdmclient {


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
// View > Sync Keyboard Language: the server follows this Mac's input source
// (docs/protocol.md, "键盘语言同步"). On unless turned off.
NSString* const kSyncKeyboardLanguageKey = @"SyncKeyboardLanguage";
NSString* const kSyncKeyboardLanguageChanged = @"RDMSyncKeyboardLanguageChanged";

// View > Data Saver (Sharp Text): text mode (docs/protocol.md, "文字模式"): the
// server sends as little as it can with text that stays sharp. Off unless
// chosen; remembered.
NSString* const kTextModeKey = @"TextMode";
NSString* const kTextModeChanged = @"RDMTextModeChanged";

bool TextMode() { return [NSUserDefaults.standardUserDefaults boolForKey:kTextModeKey]; }

// View > Play Sound from Remote Computer (docs/audio.md): ask for the
// server's sound and play it here. On unless turned off; remembered. Turning
// it off pauses the running session's sound (the server sends nothing) and
// leaves it out of later sessions; turning it on resumes it.
NSString* const kPlaySoundKey = @"PlayRemoteSound";
NSString* const kPlaySoundChanged = @"RDMPlaySoundChanged";

bool PlaySound() {
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  return [d objectForKey:kPlaySoundKey] ? [d boolForKey:kPlaySoundKey] : true;
}

// View > Observe Only (as Screen Sharing's observe mode): watch, send no
// keys, clicks or scrolling. Not remembered: every launch starts in control.
// ⌃⌥⌘O switches it, also in full screen where the menu bar is hidden.
NSString* const kObserveChanged = @"RDMObserveChanged";
bool gObserveOnly = false;  // main thread

// View > Text Mode at Standard Resolution: in text mode, ask for a desktop
// of one pixel per point even on a Retina screen. A 2x desktop has four
// times the pixels to send; at 1x the text is the server's own 1x rendering
// (not a scaled-down 2x one), shown here with every pixel doubled. On by
// default; only matters while text mode is on.
NSString* const kTextModeStandardResolutionKey = @"TextModeStandardResolution";

bool TextModeStandardResolution() {
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  return [d objectForKey:kTextModeStandardResolutionKey] ? [d boolForKey:kTextModeStandardResolutionKey] : true;
}

// Pixels per point of the desktop to ask for, on a screen of `backing`.
double DesktopScale(double backing) {
  if (backing <= 0) backing = 1;
  return TextMode() && TextModeStandardResolution() ? 1.0 : backing;
}

// View > Use All Displays in Full Screen: an extended desktop (docs/protocol.md,
// "扩展桌面"). While the remote window is full screen, each other screen of
// this Mac gets a display of its own on the server, next to the main one as
// the screens are arranged here. On unless turned off.
NSString* const kUseAllDisplaysKey = @"UseAllDisplays";
NSString* const kUseAllDisplaysChanged = @"RDMUseAllDisplaysChanged";

bool UseAllDisplays() {
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  return [d objectForKey:kUseAllDisplaysKey] ? [d boolForKey:kUseAllDisplaysKey] : true;
}

// Where `screen` is next to `home`: its top-left corner relative to home's,
// in points, y down (the server's display coordinates).
void ScreenOffset(NSScreen* home, NSScreen* screen, int32_t* x, int32_t* y) {
  *x = static_cast<int32_t>(std::lround(screen.frame.origin.x - home.frame.origin.x));
  *y = static_cast<int32_t>(std::lround(NSMaxY(home.frame) - NSMaxY(screen.frame)));
}

// The other screens as extra desktops, beside the one at `home`.
std::vector<rdm::ExtraDesktop> ExtraDesktopsFor(NSScreen* home) {
  std::vector<rdm::ExtraDesktop> out;
  for (NSScreen* screen in NSScreen.screens) {
    if (screen == home || [screen isEqual:home] || out.size() >= rdm::kMaxExtraDesktops) continue;
    rdm::ExtraDesktop e;
    const double scale = DesktopScale(screen.backingScaleFactor);
    e.desktop.width = static_cast<int32_t>(std::floor(screen.frame.size.width) * scale);
    e.desktop.height = static_cast<int32_t>(std::floor(screen.frame.size.height) * scale);
    e.desktop.scale = scale;
    ScreenOffset(home, screen, &e.x, &e.y);
    out.push_back(e);
  }
  return out;
}

// The screen at that offset from `home` (an extra display's), or nil.
NSScreen* ScreenAt(NSScreen* home, int32_t x, int32_t y) {
  for (NSScreen* screen in NSScreen.screens) {
    int32_t sx = 0, sy = 0;
    ScreenOffset(home, screen, &sx, &sy);
    if (screen != home && sx == x && sy == y) return screen;
  }
  return nil;
}

// Secure Event Input (what a focused password field turns on) blocks input
// methods for the whole login session: the input menu is greyed out and Caps
// Lock goes back to being Caps Lock, so there is no switch for the server to
// follow. Which process holds it, 0 = none.
pid_t SecureInputHolder() {
  if (!IsSecureEventInputEnabled()) return 0;
  pid_t pid = -1;  // on, by someone unknown
  if (NSDictionary* session = CFBridgingRelease(CGSessionCopyCurrentDictionary())) {
    if (NSNumber* n = session[@"kCGSSessionSecureInputPID"]) pid = n.intValue;
  }
  return pid;
}

// Lets go of it if this application holds it: the connect window's password
// field is the only thing here that takes it, and AppKit is supposed to
// give it back when the field loses focus. Returns who still holds it.
pid_t ReleaseSecureInput() {
  for (int i = 0; i < 8 && SecureInputHolder() == getpid(); ++i) DisableSecureEventInput();
  return SecureInputHolder();
}

// "Bitwarden (pid 512)", for telling the user who blocks the input methods.
NSString* DescribeProcess(pid_t pid) {
  NSRunningApplication* app = pid > 0 ? [NSRunningApplication runningApplicationWithProcessIdentifier:pid] : nil;
  return app.localizedName ? [NSString stringWithFormat:@"%@ (pid %d)", app.localizedName, pid]
                           : pid > 0 ? [NSString stringWithFormat:@"process %d", pid] : @"another process";
}

bool SyncKeyboardLanguage() {
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  return [d objectForKey:kSyncKeyboardLanguageKey] ? [d boolForKey:kSyncKeyboardLanguageKey] : true;
}

// View > Statistics: the overlay in the remote window's top right corner
// (figures from the server, docs/protocol.md "会话统计"). Clicks go through it.
NSString* const kStatsShownKey = @"StatsOverlay";
NSString* const kStatsFieldsKey = @"StatsFields";
NSString* const kStatsFieldsKnownKey = @"StatsFieldsKnown";
NSString* const kStatsOpacityKey = @"StatsOpacity";
NSString* const kStatsChanged = @"RDMStatsChanged";


bool StatsShown() {
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  return [d objectForKey:kStatsShownKey] ? [d boolForKey:kStatsShownKey] : true;
}

NSSet<NSString*>* StatsFieldsOn() {
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  NSArray* saved = [d arrayForKey:kStatsFieldsKey];
  NSMutableSet* on = [NSMutableSet set];
  if (!saved) {
    for (const auto& f : kStatsFields) {
      if (f.on_by_default) [on addObject:f.key];
    }
    return on;
  }
  [on addObjectsFromArray:saved];
  // A field added after the user made their pick (6.0: the traffic total)
  // comes on once if it is on by default; turning it off sticks.
  NSArray* known = [d arrayForKey:kStatsFieldsKnownKey];
  if (!known || ![known containsObject:@"traffic"]) {
    [on addObject:@"traffic"];
    [d setObject:on.allObjects forKey:kStatsFieldsKey];
    [d setObject:@[ @"traffic" ] forKey:kStatsFieldsKnownKey];
  }
  return on;
}

int StatsOpacity() {
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  return [d objectForKey:kStatsOpacityKey] ? static_cast<int>([d integerForKey:kStatsOpacityKey]) : 35;
}

NSString* StatsKbps(double kbps) {
  return kbps >= 1000 ? [NSString stringWithFormat:@"%.1f Mbps", kbps / 1000] : [NSString stringWithFormat:@"%.0f kbps", kbps];
}

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
  if (e == "connect_failed") text = @"连不上那台电脑：它可能没开机、不在网络上，或者服务没在运行。";
  else if (e == "bad_credentials") text = @"账号或密码不对。";
  else if (e == "not_console_user")
    text = @"那台 Mac 上现在是别人登录着，只能用那个账号连接（停在登录窗口时任何账号都可以）。";
  else if (e == "unavailable")
    text = @"那台 Mac 的服务还没准备好（它随登录窗口或用户会话启动），稍等一下再连。";
  else if (e == "rate_limited")
    text = [NSString stringWithFormat:@"失败次数太多，请 %lld 秒后再试。",
                                      static_cast<long long>((r.retry_after_ms.value_or(60000) + 999) / 1000)];
  else if (e == "busy") text = @"已经有人连着那台 Mac。";
  else if (e == "capture_denied")
    text = @"那台 Mac 不允许录制屏幕：请在它的“系统设置 → 隐私与安全性 → 屏幕录制”里打开 RDMachine Server。";
  else if (e == "no_display") text = @"那台电脑没有可以共享的显示器。";
  else if (e == "incompatible") text = @"两边的 rdmachine 版本不一样，请都更新到同一个版本。";
  else if (e == "media_failed") text = @"登录成功了，但画面连接没建立起来（可能被防火墙挡住）。";
  else if (e == "untrusted_server") text = @"已取消连接。";
  else if (e == "timeout") text = @"那台电脑没有及时回应。";
  else text = [NSString stringWithFormat:@"连接失败（%@）。", NS(e)];
  if (!r.detail.empty()) text = [text stringByAppendingFormat:@"\n\n%@", NS(r.detail)];
  return text;
}

// The address field: "host", "host:port", "[v6]:port". False if empty.
bool ParseTarget(NSString* target, std::string* host, uint16_t* port) {
  NSString* t = [target stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  if (t.length == 0) return false;
  std::string h = t.UTF8String;
  uint16_t p = rdm::kDefaultPort;
  if (!h.empty() && h[0] == '[') {
    const auto close = h.find(']');
    if (close != std::string::npos) {
      if (close + 2 < h.size() && h[close + 1] == ':') p = static_cast<uint16_t>(std::atoi(h.c_str() + close + 2));
      h = h.substr(1, close - 1);
    }
  } else if (std::count(h.begin(), h.end(), ':') == 1) {
    p = static_cast<uint16_t>(std::atoi(h.c_str() + h.find(':') + 1));
    h = h.substr(0, h.find(':'));
  }
  if (p == 0) p = rdm::kDefaultPort;
  *host = h;
  *port = p;
  return true;
}

// The login remembered for a server (docs/decisions.md, "记住密码"): after
// the first password login the account and password go into this Mac's
// login keychain, one item per server address, and every later connection
// (at launch, after a drop) logs in with them. rdmachine is for the user's
// own Macs: whoever can use this Mac may use them.
NSMutableDictionary* LoginQuery(const std::string& key) {
  return [@{
    (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService : [@"rdmachine " stringByAppendingString:NS(key)],
  } mutableCopy];
}

// Whether a login is saved, by key (main thread): the list asks for every
// row once a second, so the answer is kept until a login is saved or forgotten.
NSMutableDictionary<NSString*, NSNumber*>* SavedLoginCache() {
  static NSMutableDictionary<NSString*, NSNumber*>* cache = [NSMutableDictionary dictionary];
  return cache;
}

void ForgetLogin(const std::string& key) {
  SecItemDelete((__bridge CFDictionaryRef)LoginQuery(key));
  [SavedLoginCache() removeObjectForKey:NS(key)];
}

void SaveLogin(const std::string& key, const std::string& user, const std::string& password) {
  ForgetLogin(key);
  NSMutableDictionary* q = LoginQuery(key);
  q[(__bridge id)kSecAttrAccount] = NS(user);
  q[(__bridge id)kSecAttrLabel] = [NSString stringWithFormat:@"rdmachine: %@", NS(key)];
  q[(__bridge id)kSecValueData] = [NSData dataWithBytes:password.data() length:password.size()];
  const OSStatus st = SecItemAdd((__bridge CFDictionaryRef)q, nullptr);
  if (st != errSecSuccess) NSLog(@"rdmachine: could not remember the login for %s (%d)", key.c_str(), static_cast<int>(st));
}

bool LoadLogin(const std::string& key, std::string* user, std::string* password) {
  NSMutableDictionary* q = LoginQuery(key);
  q[(__bridge id)kSecReturnAttributes] = @YES;
  q[(__bridge id)kSecReturnData] = @YES;
  q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
  CFTypeRef result = nullptr;
  if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &result) != errSecSuccess || !result) return false;
  NSDictionary* d = CFBridgingRelease(result);
  NSString* account = d[(__bridge id)kSecAttrAccount];
  NSData* data = d[(__bridge id)kSecValueData];
  if (account.length == 0 || data.length == 0) return false;
  *user = account.UTF8String;
  password->assign(static_cast<const char*>(data.bytes), data.length);
  return true;
}

// The computers connected to before, most recent first (docs/decisions.md,
// "便捷优先"): address, account, the name the server gave, when. Passwords
// are not in here but in the keychain (SaveLogin), by address.
NSString* const kMachinesKey = @"machines";

NSArray<NSDictionary*>* LoadMachines() {
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  NSArray* list = [d arrayForKey:kMachinesKey];
  if (!list) {
    // Before the list (3.1.0 builds): the last computer, if any.
    NSString* last = [d stringForKey:@"lastHost"];
    if (last.length == 0) return @[];
    list = @[ @{@"target" : last, @"user" : [d stringForKey:@"lastUser"] ?: @"", @"name" : last, @"last" : NSDate.date} ];
    [d setObject:list forKey:kMachinesKey];
  }
  NSMutableArray* ok = [NSMutableArray array];
  for (id m in list) {
    if ([m isKindOfClass:NSDictionary.class] && [m[@"target"] isKindOfClass:NSString.class]) [ok addObject:m];
  }
  return ok;
}

void SaveMachines(NSArray<NSDictionary*>* list) { [NSUserDefaults.standardUserDefaults setObject:list forKey:kMachinesKey]; }


// After a connection worked: to the top of the list, added if new.
void RememberMachine(NSString* target, NSString* user, NSString* name) {
  NSMutableArray* list = [LoadMachines() mutableCopy];
  NSIndexSet* same = [list indexesOfObjectsPassingTest:^BOOL(NSDictionary* m, NSUInteger, BOOL*) {
    // "mini.local" and "mini.local:4100" are one computer (one saved login).
    if ([m[@"target"] caseInsensitiveCompare:target] == NSOrderedSame) return YES;
    const std::string a = MachineKey(m[@"target"]);
    return !a.empty() && a == MachineKey(target);
  }];
  // What the user gave it (alias, group) stays across connections.
  NSDictionary* before = same.count ? list[same.firstIndex] : nil;
  [list removeObjectsAtIndexes:same];
  NSMutableDictionary* m = [@{@"target" : target, @"user" : user ?: @"", @"name" : name.length ? name : target,
                              @"last" : NSDate.date} mutableCopy];
  if ([before[@"alias"] length]) m[@"alias"] = before[@"alias"];
  if ([before[@"group"] length]) m[@"group"] = before[@"group"];
  [list insertObject:m atIndex:0];
  SaveMachines(list);
}

// The name a computer is listed under: the alias the user gave it, else the
// name its server reported, else its address.
NSString* MachineTitle(NSDictionary* m) {
  if ([m[@"alias"] length]) return m[@"alias"];
  if ([m[@"name"] length]) return m[@"name"];
  return m[@"target"] ?: @"";
}

// The list in groups (View > Group Computers; off unless the user turns it on).
NSString* const kMachinesGroupedKey = @"machinesGrouped";
bool MachinesGrouped() { return [NSUserDefaults.standardUserDefaults boolForKey:kMachinesGroupedKey]; }

std::string MachineKey(NSString* target) {
  std::string host;
  uint16_t port = 0;
  return ParseTarget(target, &host, &port) ? rdm::KnownHosts::Key(host, port) : std::string();
}

// Without reading the password: only whether the item is there (no secret
// in memory, and nothing for the keychain to ask permission for).
bool HasSavedLogin(NSString* target) {
  const std::string key = MachineKey(target);
  if (key.empty()) return false;
  NSString* k = NS(key);
  if (NSNumber* known = SavedLoginCache()[k]) return known.boolValue;
  NSMutableDictionary* q = LoginQuery(key);
  q[(__bridge id)kSecReturnAttributes] = @YES;
  q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
  CFTypeRef result = nullptr;
  const bool has = SecItemCopyMatching((__bridge CFDictionaryRef)q, &result) == errSecSuccess && result;
  if (result) CFRelease(result);
  SavedLoginCache()[k] = @(has);
  return has;
}

// One clipboard for every session of this app: each session's sync then
// knows a promise another session put there (the copy of another Mac) and
// leaves it alone instead of reading it, which would fetch it at once.
std::shared_ptr<rdm::Clipboard> SharedClipboard() {
  static std::shared_ptr<rdm::Clipboard> clipboard = rdm::SystemClipboard();
  return clipboard;
}

// Whether something accepts TCP connections at host:port within timeout_ms:
// the list's "online" (the server's control port; nothing is sent, and the
// server does not log a peer that closes without a word).
bool TcpReachable(const std::string& host, uint16_t port, int timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return false;
  bool ok = false;
  for (addrinfo* a = res; a && !ok; a = a->ai_next) {
    const int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) continue;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
      ok = true;
    } else if (errno == EINPROGRESS) {
      pollfd pfd{fd, POLLOUT, 0};
      if (poll(&pfd, 1, timeout_ms) == 1) {
        int err = 0;
        socklen_t len = sizeof err;
        ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
      }
    }
    close(fd);
  }
  freeaddrinfo(res);
  return ok;
}

// "12 秒", "5 分钟", "2 小时 5 分钟".
NSString* DurationText(int64_t secs) {
  if (secs < 60) return [NSString stringWithFormat:@"%lld 秒", static_cast<long long>(std::max<int64_t>(secs, 1))];
  if (secs < 3600) return [NSString stringWithFormat:@"%lld 分钟", static_cast<long long>(secs / 60)];
  const long long h = secs / 3600, m = (secs % 3600) / 60;
  return m ? [NSString stringWithFormat:@"%lld 小时 %lld 分钟", h, m] : [NSString stringWithFormat:@"%lld 小时", h];
}

// How the remote window was left (full screen, or its size), for the next
// connection.
NSString* const kRemoteFullScreenKey = @"remoteWindowFullScreen";
NSString* const kRemoteSizeKey = @"remoteWindowContentSize";


}  // namespace rdmclient
