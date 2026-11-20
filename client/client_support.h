// What the client's files share: settings (View menu), the saved computers and
// logins, and small helpers. Split out of main.mm (6.0.2); see main.mm for what
// the client does.
#pragma once

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>  // kTISNotifySelectedKeyboardInputSourceChanged
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

#include <rdaccel/rdaccel.h>
#import <Security/Security.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <optional>
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

#include "rdmachine/desktop.h"
#include "rdmachine/ime_wire.h"
#include "rdmachine/input_source_sync.h"
#include "rdmachine/audio_control.h"
#include "rdmachine/picture_mode.h"
#include "rdmachine/protocol.h"
#include "rdmachine/session_stats.h"
#include "rdmachine/session/config_store.h"
#include "rdmachine/session/remote_session.h"
#include "rdmachine/session/system_clipboard.h"
#include "rdmachine/session/file_progress.h"
#include "rdmachine/session/system_files.h"

namespace rdmclient {

struct StatsField {
  NSString* key;
  NSString* title;
  bool on_by_default;
};

inline const StatsField kStatsFields[] = {
    {@"time", @"连接时长", true},
    {@"fps", @"帧率", true},        {@"latency", @"延迟", true},
    {@"loss", @"丢包", true},      {@"bitrate", @"码率", false},
    {@"bandwidth", @"带宽", false},  {@"resolution", @"分辨率", false},
    {@"encoder", @"编码器", false},      {@"input", @"输入法", false},
    {@"response", @"操作到画面", true},  {@"audio", @"声音", false},
    {@"traffic", @"流量累计", true},
};

inline const int kStatsOpacities[] = {15, 35, 60};  // background, percent

NSString* NS(const std::string& s);
uint32_t Modifiers(NSEventModifierFlags f);
extern NSString* const kSyncKeyboardLanguageKey;
extern NSString* const kSyncKeyboardLanguageChanged;
extern NSString* const kUseLocalInputMethodKey;
extern NSString* const kUseLocalInputMethodChanged;
extern NSString* const kTextModeKey;
extern NSString* const kTextModeChanged;
bool TextMode();
extern NSString* const kPlaySoundKey;
extern NSString* const kPlaySoundChanged;
bool PlaySound();
extern NSString* const kObserveChanged;
extern bool gObserveOnly;  // main thread
extern NSString* const kTextModeStandardResolutionKey;
bool TextModeStandardResolution();
double DesktopScale(double backing);
extern NSString* const kUseAllDisplaysKey;
extern NSString* const kUseAllDisplaysChanged;
bool UseAllDisplays();
void ScreenOffset(NSScreen* home, NSScreen* screen, int32_t* x, int32_t* y);
std::vector<rdm::ExtraDesktop> ExtraDesktopsFor(NSScreen* home);
NSScreen* ScreenAt(NSScreen* home, int32_t x, int32_t y);
pid_t SecureInputHolder();
pid_t ReleaseSecureInput();
NSString* DescribeProcess(pid_t pid);
bool SyncKeyboardLanguage();
bool UseLocalInputMethod();
extern NSString* const kStatsShownKey;
extern NSString* const kStatsFieldsKey;
extern NSString* const kStatsFieldsKnownKey;
extern NSString* const kStatsOpacityKey;
extern NSString* const kStatsChanged;
bool StatsShown();
NSSet<NSString*>* StatsFieldsOn();
int StatsOpacity();
NSString* StatsKbps(double kbps);
NSEventModifierFlags FlagOfModifierKey(unsigned short keyCode);
uint32_t ButtonOf(NSEvent* e);
rda_input_event Event(rda_input_kind kind);
IOSurfaceRef MakeSurface(int32_t w, int32_t h);
void Owe(std::vector<rda_rect>& owed, const rda_rect& r, int32_t w, int32_t h);
NSString* DescribeError(const rdm::ConnectResult& r);
bool ParseTarget(NSString* target, std::string* host, uint16_t* port);
NSMutableDictionary* LoginQuery(const std::string& key);
NSMutableDictionary<NSString*, NSNumber*>* SavedLoginCache();
void ForgetLogin(const std::string& key);
void SaveLogin(const std::string& key, const std::string& user, const std::string& password);
bool LoadLogin(const std::string& key, std::string* user, std::string* password);
extern NSString* const kMachinesKey;
NSArray<NSDictionary*>* LoadMachines();
void SaveMachines(NSArray<NSDictionary*>* list);
void RememberMachine(NSString* target, NSString* user, NSString* name);
NSString* MachineTitle(NSDictionary* m);
extern NSString* const kMachinesGroupedKey;
bool MachinesGrouped();
std::string MachineKey(NSString* target);
bool HasSavedLogin(NSString* target);
std::shared_ptr<rdm::Clipboard> SharedClipboard();
bool TcpReachable(const std::string& host, uint16_t port, int timeout_ms);
NSString* DurationText(int64_t secs);
extern NSString* const kRemoteFullScreenKey;
extern NSString* const kRemoteSizeKey;

}  // namespace rdmclient
