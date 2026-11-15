// The app's side of the File Provider extension (fileprovider/): adds the
// domain, connects to the extension's service and translates between
// core's FileTarget (the clipboard's thread) and the extension (XPC).
#import <AppKit/AppKit.h>
#import <FileProvider/FileProvider.h>
#import <Foundation/Foundation.h>

#include <map>
#include <mutex>
#include <set>

#include "rdm_files_xpc.h"
#include "rdmachine/session/system_files.h"

namespace rdm {
class MacFileTarget;
}

namespace {

constexpr double kWaitSeconds = 5;

NSString* NS(const std::string& s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]; }
std::string Std(NSString* s) { return s.UTF8String ? std::string(s.UTF8String) : std::string(); }

void Log(const std::string& line) { NSLog(@"files: %@", NS(line)); }

bool HasExtension() {
  NSURL* plugins = NSBundle.mainBundle.builtInPlugInsURL;
  if (!plugins) return false;
  for (NSURL* u in [NSFileManager.defaultManager contentsOfDirectoryAtURL:plugins includingPropertiesForKeys:nil options:0 error:nil]) {
    NSDictionary* info = [NSBundle bundleWithURL:u].infoDictionary;
    if ([info[@"NSExtension"][@"NSExtensionPointIdentifier"] isEqual:@"com.apple.fileprovider-nonui"]) return true;
  }
  return false;
}

}  // namespace

// Everything shared by the process's sessions: the domain, the connection to
// the extension and what is on offer. Guarded by `mu`.
@interface RDMFilesHub : NSObject <RDMFilesHost>
+ (instancetype)shared;
@end

namespace rdm {

class MacFileTarget : public FileTarget {
 public:
  explicit MacFileTarget(std::string peer) : peer(std::move(peer)) {}
  ~MacFileTarget() override;
  void SetRequester(FileRequester* requester) override;
  void DropRequester(FileRequester* requester) override;
  std::optional<std::vector<std::string>> Publish(uint64_t announce, const std::vector<FileEntry>& items) override;
  void Withdraw(uint64_t announce) override;
  void OnNamesPasted() override;
  void OnList(uint64_t announce, uint32_t item, const std::vector<FileEntry>& entries, const std::string& error) override;
  void OnFileStarted(uint64_t announce, uint32_t item, const FileEntry& entry, const std::string& local_path) override;
  void OnProgress(uint64_t announce, uint32_t item, const std::string& path, uint64_t bytes) override;
  void OnFileReady(uint64_t announce, uint32_t item, const FileEntry& entry, const std::string& local_path) override;
  void OnItemDone(uint64_t announce, uint32_t item, const std::string& error) override;

  const std::string peer;
  FileRequester* requester = nullptr;  // under the hub's lock
};

}  // namespace rdm

namespace {

// A copy that is no longer on the clipboard and has nothing under way stays
// this long (Finder may still ask for more of it), then goes.
constexpr double kLingerSeconds = 60;
// A fetch cut short by a dropped connection waits this long for the session
// to come back (the client reconnects within a minute).
constexpr double kOrphanSeconds = 90;
// The prompt to turn RDMachine on comes at most this often.
constexpr double kPromptEverySeconds = 600;

// One copy of the peer's that the extension shows (its folder "c<time>-<n>").
struct Copy {
  std::string id;
  uint64_t announce = 0;
  std::string peer;
  rdm::MacFileTarget* target = nullptr;  // who published it (null: gone)
  rdm::FileRequester* fetcher = nullptr;  // who fetches its items (null: no one now)
  std::vector<rdm::FileEntry> items;
  NSData* json = nil;  // what was published (again after the extension restarts)
  bool on_clipboard = true;
  std::set<std::string> requested;            // files the extension waits for
  std::map<std::string, std::string> ready;   // files that arrived first
  // Files on their way: where they are written and how much is there.
  std::map<std::string, std::pair<std::string, uint64_t>> growing;
  std::set<uint32_t> active;  // items being fetched
  CFAbsoluteTime last_used = 0;
  uint64_t timer = 0;  // the pending linger or orphan check (a newer one replaces it)

  bool Busy() const { return !requested.empty() || !active.empty(); }
};

struct Hub {
  std::mutex mu;
  NSXPCConnection* conn = nil;
  NSURL* root = nil;
  NSFileProviderDomain* domain = nil;
  std::map<std::string, Copy> copies;            // by copy id
  std::map<uint64_t, std::string> by_announce;   // announce -> copy id
  std::set<rdm::MacFileTarget*> targets;
  uint64_t counter = 0, timers = 0;
  // After a failed connection (RDMachine turned off in System Settings), no
  // new try for a while: each try may wait seconds on the clipboard's thread.
  NSDate* retry_after = nil;
  std::string last_error;
  CFAbsoluteTime prompted = 0;
};

Hub& TheHub() {
  static Hub* h = new Hub;
  return *h;
}

std::string ItemId(const std::string& copy_id, uint32_t item, const std::string& path = {}) {
  std::string id = copy_id + "/" + std::to_string(item);
  if (!path.empty()) id += "/" + path;
  return id;
}

// "c3/0/sub/a.txt" -> ("c3", 0); false if malformed.
bool ParseItemId(const std::string& id, std::string* copy_id, uint32_t* item) {
  const size_t a = id.find('/');
  if (a == std::string::npos) return false;
  const size_t b = id.find('/', a + 1);
  const std::string idx = id.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
  if (idx.empty() || idx.size() > 9 || idx.find_first_not_of("0123456789") != std::string::npos) return false;
  *copy_id = id.substr(0, a);
  *item = static_cast<uint32_t>(std::stoul(idx));
  return true;
}

Copy* ByAnnounce(Hub& h, uint64_t announce) {
  auto it = h.by_announce.find(announce);
  if (it == h.by_announce.end()) return nullptr;
  auto c = h.copies.find(it->second);
  return c == h.copies.end() ? nullptr : &c->second;
}

id<RDMFilesExtension> Proxy(NSXPCConnection* c) {
  return [c remoteObjectProxyWithErrorHandler:^(NSError* e) { Log("the extension did not answer: " + Std(e.localizedDescription)); }];
}

// Waits for an asynchronous call (the clipboard's thread; never the main
// thread, where the answers may need to arrive).
bool Wait(void (^start)(dispatch_semaphore_t)) {
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  start(sem);
  return dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kWaitSeconds * NSEC_PER_SEC))) == 0;
}

// ---- keeping copies --------------------------------------------------------------

void Check(const std::string& copy_id, uint64_t timer);

// Comes back to the copy after `seconds` (replacing an earlier check).
void Schedule(Hub& h, Copy& c, double seconds) {
  const uint64_t timer = ++h.timers;
  c.timer = timer;
  const std::string id = c.id;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ Check(id, timer); });
}

// The extension no longer shows the copy and what was fetched for it goes.
void Retire(Hub& h, const std::string& copy_id) {
  auto it = h.copies.find(copy_id);
  if (it == h.copies.end()) return;
  Copy& c = it->second;
  if (h.conn) {
    [Proxy(h.conn) withdrawCopy:NS(c.id)];
    for (const auto& id : c.requested) [Proxy(h.conn) itemFailed:NS(id) reason:@"no longer on offer"];
  }
  rdm::RemoveStagedCopy(rdm::FileStagingDir(), c.announce);
  h.by_announce.erase(c.announce);
  h.copies.erase(it);
}

// After something changed: a copy off the clipboard with nothing under way
// lingers a while, then goes.
void Settle(Hub& h, Copy& c) {
  if (c.on_clipboard || c.Busy()) return;
  Schedule(h, c, kLingerSeconds);
}

// Everything the extension waits for of the copy fails.
void FailAll(Hub& h, Copy& c, NSString* reason) {
  if (h.conn) {
    for (const auto& id : c.requested) [Proxy(h.conn) itemFailed:NS(id) reason:reason];
  }
  c.requested.clear();
  c.active.clear();
}

void Check(const std::string& copy_id, uint64_t timer) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  auto it = h.copies.find(copy_id);
  if (it == h.copies.end() || it->second.timer != timer) return;
  Copy& c = it->second;
  if (c.Busy() && !c.fetcher) {
    // Cut short, and no session came back for it.
    Log("gave up fetching " + c.id + ": not connected");
    FailAll(h, c, @"RDMachine is not connected");
    c.on_clipboard = false;
  }
  if (c.on_clipboard || c.Busy()) return;
  if (CFAbsoluteTimeGetCurrent() - c.last_used < kLingerSeconds - 1) {
    Schedule(h, c, kLingerSeconds);
    return;
  }
  Retire(h, copy_id);
}

std::string NameOf(const Copy& c, uint32_t item) { return item < c.items.size() ? c.items[item].path : std::string(); }

// Asks `r` for what the copy waits for (a session that came back, or took
// over): each item once; it goes on where the last one stopped.
void Resume(Copy& c, rdm::FileRequester* r) {
  c.fetcher = r;
  std::set<uint32_t> items = c.active;
  for (const auto& id : c.requested) {
    std::string copy_id;
    uint32_t item = 0;
    if (ParseItemId(id, &copy_id, &item)) items.insert(item);
  }
  for (uint32_t item : items) {
    c.active.insert(item);
    r->RequestFileFetch(c.announce, item, NameOf(c, item));
  }
  if (!items.empty()) Log("going on with " + c.id + " in the new session");
}

// A requester of `peer`'s that can fetch, if any (the copy's own target's first).
rdm::FileRequester* RequesterFor(Hub& h, const Copy& c) {
  if (c.target && c.target->requester) return c.target->requester;
  for (rdm::MacFileTarget* t : h.targets) {
    if (t->requester && t->peer == c.peer) return t->requester;
  }
  return nullptr;
}

// Copies without a fetcher that `t`'s new requester can take.
void Adopt(Hub& h, rdm::MacFileTarget* t) {
  if (!t->requester) return;
  for (auto& [id, c] : h.copies) {
    if (c.fetcher || c.peer != t->peer) continue;
    if (!c.target) c.target = t;
    if (c.Busy()) Resume(c, t->requester);
  }
}

// ---- the prompt ---------------------------------------------------------------------

// RDMachine is turned off in System Settings, and an application just pasted
// the peer's file names instead of the files: say how to turn it on.
void Prompt() {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    CFOptionFlags response = 0;
    CFUserNotificationDisplayAlert(
        0, kCFUserNotificationNoteAlertLevel, nullptr, nullptr, nullptr, CFSTR("要粘贴文件，请先打开 RDMachine"),
        (__bridge CFStringRef) @"另一台 Mac 复制的是文件，这台 Mac 上还不能粘贴文件，所以只粘贴了文件名。\n\n"
                                @"在“系统设置 → 通用 → 登录项与扩展”里打开 RDMachine（在“文件提供程序”下面），"
                                @"之后在另一台 Mac 上重新复制一次，就能粘贴文件了。只需要打开一次。",
        CFSTR("打开设置"), CFSTR("以后再说"), nullptr, &response);
    if ((response & 0x3) == kCFUserNotificationDefaultResponse) {
      {
        // Try again at the next copy, not after the usual back-off.
        Hub& h = TheHub();
        std::lock_guard lock(h.mu);
        h.retry_after = nil;
      }
      NSURL* u = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.LoginItems-Settings.extension"];
      dispatch_async(dispatch_get_main_queue(), ^{ [NSWorkspace.sharedWorkspace openURL:u]; });
    }
  });
}

// ---- the extension ------------------------------------------------------------------

// Adds the domain and connects to the extension. With the lock held.
bool ConnectOnce(Hub& h, std::string* error);

bool Connect(Hub& h, std::string* error) {
  if (h.conn) return true;
  if (h.retry_after && h.retry_after.timeIntervalSinceNow > 0) {
    *error = h.last_error;
    return false;
  }
  if (ConnectOnce(h, error)) {
    h.retry_after = nil;
    h.last_error.clear();
    // A new extension process knows nothing: show the copies again.
    for (auto& [id, c] : h.copies) {
      [Proxy(h.conn) publish:c.json reply:^(NSString*){}];
    }
    return true;
  }
  h.last_error = *error;
  h.retry_after = [NSDate dateWithTimeIntervalSinceNow:30];
  return false;
}

bool ConnectOnce(Hub& h, std::string* error) {
  if (!h.domain) {
    NSFileProviderDomain* d = [[NSFileProviderDomain alloc] initWithIdentifier:@"files" displayName:@"RDMachine"];
    __block NSError* err = nil;
    if (!Wait(^(dispatch_semaphore_t sem) {
          [NSFileProviderManager addDomain:d
                         completionHandler:^(NSError* e) {
                           err = e;
                           dispatch_semaphore_signal(sem);
                         }];
        })) {
      *error = "adding the File Provider domain timed out";
      return false;
    }
    if (err) {
      *error = "cannot add the File Provider domain: " + Std(err.localizedDescription);
      return false;
    }
    h.domain = d;
  }
  if (!h.root) {
    __block NSURL* root = nil;
    __block NSError* err = nil;
    NSFileProviderManager* m = [NSFileProviderManager managerForDomain:h.domain];  // (captured below)
    if (!Wait(^(dispatch_semaphore_t sem) {
          [m getUserVisibleURLForItemIdentifier:NSFileProviderRootContainerItemIdentifier
                              completionHandler:^(NSURL* u, NSError* e) {
                                root = u;
                                err = e;
                                dispatch_semaphore_signal(sem);
                              }];
        }) || !root) {
      *error = "the RDMachine location is not available (turn it on in System Settings > General > Login Items & "
               "Extensions)" + (err ? ": " + Std(err.localizedDescription) : std::string());
      return false;
    }
    h.root = root;
  }
  __block NSFileProviderService* service = nil;
  __block NSError* err = nil;
  NSURL* root_url = h.root;
  if (!Wait(^(dispatch_semaphore_t sem) {
        [NSFileManager.defaultManager
            getFileProviderServicesForItemAtURL:root_url
                              completionHandler:^(NSDictionary<NSFileProviderServiceName, NSFileProviderService*>* s, NSError* e) {
                                service = s[RDMFilesServiceName];
                                err = e;
                                dispatch_semaphore_signal(sem);
                              }];
      }) || !service) {
    *error = "the RDMachine location is turned off or not ready" + (err ? ": " + Std(err.localizedDescription) : std::string());
    return false;
  }
  __block NSXPCConnection* conn = nil;
  if (!Wait(^(dispatch_semaphore_t sem) {
        [service getFileProviderConnectionWithCompletionHandler:^(NSXPCConnection* c, NSError* e) {
          conn = c;
          err = e;
          dispatch_semaphore_signal(sem);
        }];
      }) || !conn) {
    *error = "cannot connect to the extension" + (err ? ": " + Std(err.localizedDescription) : std::string());
    return false;
  }
  conn.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(RDMFilesExtension)];
  conn.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(RDMFilesHost)];
  conn.exportedObject = [RDMFilesHub shared];
  __weak NSXPCConnection* weak = conn;
  auto lost = ^{
    Hub& hh = TheHub();
    std::lock_guard lock(hh.mu);
    if (hh.conn == weak) hh.conn = nil;
    // Waiting files will not arrive at a new extension process.
    for (auto& [id, c] : hh.copies) c.requested.clear();
  };
  conn.invalidationHandler = lost;
  conn.interruptionHandler = lost;
  [conn resume];
  h.conn = conn;
  Log("connected to the extension");
  return true;
}

// What has arrived of a file still on its way, for the extension to hand on
// in pieces (Finder copies as it comes).
void DeliverGrowing(Hub& h, const std::string& item_id, const std::string& local_path, uint64_t bytes) {
  NSFileHandle* fh = [NSFileHandle fileHandleForReadingAtPath:NS(local_path)];
  if (!fh) return;
  [Proxy(h.conn) fileGrowing:NS(item_id) handle:fh];
  if (bytes > 0) [Proxy(h.conn) progress:NS(item_id) bytes:bytes];
}

void Deliver(Hub& h, const std::string& item_id, const std::string& local_path) {
  NSFileHandle* fh = [NSFileHandle fileHandleForReadingAtPath:NS(local_path)];
  if (!fh) {
    [Proxy(h.conn) itemFailed:NS(item_id) reason:@"the fetched file cannot be read"];
    return;
  }
  [Proxy(h.conn) fileReady:NS(item_id) handle:fh];
}

}  // namespace

@implementation RDMFilesHub
+ (instancetype)shared {
  static RDMFilesHub* hub;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ hub = [[RDMFilesHub alloc] init]; });
  return hub;
}

- (void)listItem:(NSString*)topItemID {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  std::string copy_id;
  uint32_t item = 0;
  auto it = ParseItemId(Std(topItemID), &copy_id, &item) ? h.copies.find(copy_id) : h.copies.end();
  rdm::FileRequester* r = it == h.copies.end() ? nullptr : (it->second.fetcher ?: RequesterFor(h, it->second));
  if (!r) {
    NSString* why = it == h.copies.end() ? @"gone" : @"RDMachine is not connected";
    if (h.conn) [Proxy(h.conn) setTree:[NSJSONSerialization dataWithJSONObject:@{@"item" : topItemID, @"error" : why} options:0 error:nil]];
    return;
  }
  it->second.last_used = CFAbsoluteTimeGetCurrent();
  r->RequestFileList(it->second.announce, item);
}

- (void)fetchItem:(NSString*)itemID {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  const std::string id = Std(itemID);
  std::string copy_id;
  uint32_t item = 0;
  auto it = ParseItemId(id, &copy_id, &item) ? h.copies.find(copy_id) : h.copies.end();
  if (it == h.copies.end() || item >= it->second.items.size()) {
    if (h.conn) [Proxy(h.conn) itemFailed:itemID reason:@"no longer on offer"];
    return;
  }
  Copy& c = it->second;
  c.last_used = CFAbsoluteTimeGetCurrent();
  if (auto r = c.ready.find(id); r != c.ready.end()) {
    Deliver(h, id, r->second);
    return;
  }
  if (auto g = c.growing.find(id); g != c.growing.end()) DeliverGrowing(h, id, g->second.first, g->second.second);
  c.requested.insert(id);
  if (!c.fetcher) c.fetcher = RequesterFor(h, c);
  if (!c.fetcher) {
    // Not connected right now: wait a while for the session to come back.
    Schedule(h, c, kOrphanSeconds);
    return;
  }
  c.active.insert(item);
  c.fetcher->RequestFileFetch(c.announce, item, NameOf(c, item));
}
@end

namespace rdm {

MacFileTarget::~MacFileTarget() {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  h.targets.erase(this);
  for (auto& [id, c] : h.copies) {
    if (c.target == this) c.target = nullptr;
  }
}

void MacFileTarget::SetRequester(FileRequester* r) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  h.targets.insert(this);
  requester = r;
  Adopt(h, this);
}

void MacFileTarget::DropRequester(FileRequester* r) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  if (requester == r) requester = nullptr;
  for (auto& [id, c] : h.copies) {
    if (c.fetcher != r) continue;
    c.fetcher = nullptr;
    if (!c.Busy()) continue;
    // Cut short: another session of the same peer goes on with it (one that
    // took over), or the next one does if it comes in time.
    if (rdm::FileRequester* next = RequesterFor(h, c)) {
      Resume(c, next);
    } else {
      Schedule(h, c, kOrphanSeconds);
    }
  }
}

std::optional<std::vector<std::string>> MacFileTarget::Publish(uint64_t announce, const std::vector<FileEntry>& items) {
  @autoreleasepool {
    Hub& h = TheHub();
    std::unique_lock lock(h.mu);
    std::string error;
    if (!Connect(h, &error)) {
      Log("copied files are pasted as names: " + error);
      return std::nullopt;
    }
    if (Copy* old = ByAnnounce(h, announce)) {
      // The same copy again (announced after a reconnect): as it is.
      old->on_clipboard = true;
      std::vector<std::string> paths;
      for (const auto& e : old->items) paths.push_back(Std(h.root.path) + "/" + old->id + "/" + e.path);
      return paths;
    }
    // Unique across runs: the system remembers items by identifier, and an
    // earlier run's "c1/0" (a failed fetch, in back-off) would stand in for
    // this one's.
    const std::string copy_id = "c" + std::to_string(static_cast<long long>(time(nullptr))) + "-" + std::to_string(++h.counter);
    NSData* entries = [NS(EncodeFileEntries(items)) dataUsingEncoding:NSUTF8StringEncoding];
    NSMutableDictionary* d = [NSMutableDictionary dictionary];
    d[@"copy"] = NS(copy_id);
    d[@"items"] = [NSJSONSerialization JSONObjectWithData:entries options:0 error:nil] ?: @[];
    NSData* json = [NSJSONSerialization dataWithJSONObject:d options:0 error:nil];
    NSXPCConnection* conn = h.conn;
    lock.unlock();  // the reply may need the hub (another session's callbacks)
    __block NSString* failed = @"no answer";
    const bool answered = Wait(^(dispatch_semaphore_t sem) {
      [[conn remoteObjectProxyWithErrorHandler:^(NSError* e) {
        failed = e.localizedDescription;
        dispatch_semaphore_signal(sem);
      }] publish:json
           reply:^(NSString* err) {
             failed = err;
             dispatch_semaphore_signal(sem);
           }];
    });
    lock.lock();
    if (!answered || failed) {
      Log("the extension did not take the copy: " + (failed ? Std(failed) : std::string("timed out")));
      return std::nullopt;
    }
    Copy& c = h.copies[copy_id];
    c.id = copy_id;
    c.announce = announce;
    c.peer = peer;
    c.target = this;
    c.fetcher = requester;
    c.items = items;
    c.json = json;
    c.last_used = CFAbsoluteTimeGetCurrent();
    h.by_announce[announce] = copy_id;
    h.targets.insert(this);
    std::vector<std::string> paths;
    const std::string root = Std(h.root.path);
    for (const auto& e : items) paths.push_back(root + "/" + copy_id + "/" + e.path);
    // The copy's folder shows its items on disk only once it is populated
    // (until then they wait for their parent). Listing it does that from a
    // Terminal, but not from the server running as a launchd agent: without
    // Full Disk Access the system will not populate a folder in
    // ~/Library/CloudStorage for it, and the paste then gets a file URL that
    // does not exist (Finder pastes the names as a text clipping). Asking the
    // File Provider to download the folder populates it whoever asks.
    NSString* folder = NS(root + "/" + copy_id);
    NSString* folder_id = NS(copy_id);
    NSFileProviderDomain* domain = h.domain;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      if (@available(macOS 13.0, *)) {
        [[NSFileProviderManager managerForDomain:domain]
            requestDownloadForItemWithIdentifier:folder_id
                                  requestedRange:NSMakeRange(NSNotFound, 0)
                               completionHandler:^(NSError* e) {
                                 if (e) Log("populating the copy's folder: " + Std(e.localizedDescription));
                               }];
      }
      for (int i = 0; i < 50; ++i) {
        NSArray* names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:nil];
        if (names.count > 0) return;
        usleep(100000);
      }
      Log("the copy's items did not appear on disk within 5 s");
    });
    return paths;
  }
}

void MacFileTarget::Withdraw(uint64_t announce) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  Copy* c = ByAnnounce(h, announce);
  if (!c) return;
  c->on_clipboard = false;
  Settle(h, *c);
}

void MacFileTarget::OnNamesPasted() {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  if (h.conn || h.last_error.empty()) return;  // not because RDMachine is off
  const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
  if (h.prompted != 0 && now - h.prompted < kPromptEverySeconds) return;
  h.prompted = now;
  Log("files were pasted as names; asking to turn RDMachine on");
  Prompt();
}

void MacFileTarget::OnList(uint64_t announce, uint32_t item, const std::vector<FileEntry>& entries,
                           const std::string& error) {
  @autoreleasepool {
    Hub& h = TheHub();
    std::lock_guard lock(h.mu);
    Copy* c = ByAnnounce(h, announce);
    if (!c || !h.conn) return;
    NSMutableDictionary* d = [NSMutableDictionary dictionary];
    d[@"item"] = NS(ItemId(c->id, item));
    if (!error.empty()) {
      d[@"error"] = NS(error);
    } else {
      NSData* j = [NS(EncodeFileEntries(entries)) dataUsingEncoding:NSUTF8StringEncoding];
      d[@"entries"] = [NSJSONSerialization JSONObjectWithData:j options:0 error:nil] ?: @[];
    }
    [Proxy(h.conn) setTree:[NSJSONSerialization dataWithJSONObject:d options:0 error:nil]];
  }
}

void MacFileTarget::OnFileStarted(uint64_t announce, uint32_t item, const FileEntry& entry,
                                  const std::string& local_path) {
  @autoreleasepool {
    Hub& h = TheHub();
    std::lock_guard lock(h.mu);
    Copy* c = ByAnnounce(h, announce);
    if (!c) return;
    const std::string id = ItemId(c->id, item, entry.path);
    c->growing[id] = {local_path, entry.offset};
    if (c->requested.count(id) && h.conn) DeliverGrowing(h, id, local_path, entry.offset);
  }
}

void MacFileTarget::OnProgress(uint64_t announce, uint32_t item, const std::string& path, uint64_t bytes) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  Copy* c = ByAnnounce(h, announce);
  if (!c) return;
  const std::string id = ItemId(c->id, item, path);
  if (auto it = c->growing.find(id); it != c->growing.end()) it->second.second = bytes;
  if (c->requested.count(id) && h.conn) [Proxy(h.conn) progress:NS(id) bytes:bytes];
}

void MacFileTarget::OnFileReady(uint64_t announce, uint32_t item, const FileEntry& entry, const std::string& local_path) {
  if (entry.kind != FileKind::kFile) return;  // folders and links are metadata only
  @autoreleasepool {
    Hub& h = TheHub();
    std::lock_guard lock(h.mu);
    Copy* c = ByAnnounce(h, announce);
    if (!c) return;
    const std::string id = ItemId(c->id, item, entry.path);
    c->growing.erase(id);
    c->ready[id] = local_path;  // for a later paste of the same copy, too
    if (c->requested.erase(id) && h.conn) Deliver(h, id, local_path);
  }
}

void MacFileTarget::OnItemDone(uint64_t announce, uint32_t item, const std::string& error) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  Copy* c = ByAnnounce(h, announce);
  if (!c) return;
  c->active.erase(item);
  const std::string prefix = ItemId(c->id, item);
  for (auto it = c->requested.begin(); it != c->requested.end();) {
    if (*it == prefix || it->rfind(prefix + "/", 0) == 0) {
      if (h.conn) [Proxy(h.conn) itemFailed:NS(*it) reason:NS(error.empty() ? "not in what the peer sent" : error)];
      it = c->requested.erase(it);
    } else {
      ++it;
    }
  }
  Settle(h, *c);
}

bool RegisterFileDomain(std::string* error) {
  if (!HasExtension()) {
    *error = "this app has no File Provider extension";
    return false;
  }
  NSFileProviderDomain* d = [[NSFileProviderDomain alloc] initWithIdentifier:@"files" displayName:@"RDMachine"];
  __block NSError* err = nil;
  if (!Wait(^(dispatch_semaphore_t sem) {
        [NSFileProviderManager addDomain:d
                       completionHandler:^(NSError* e) {
                         err = e;
                         dispatch_semaphore_signal(sem);
                       }];
      })) {
    *error = "timed out";
    return false;
  }
  if (err) {
    *error = Std(err.localizedDescription);
    return false;
  }
  return true;
}

std::shared_ptr<FileTarget> SystemFileTarget(const std::string& peer) {
  static const bool has = HasExtension();
  if (!has) return nullptr;
  return std::make_shared<MacFileTarget>(peer);
}

std::string FileStagingDir() {
  static const std::string dir = [] {
    NSURL* caches = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    NSString* bundle = NSBundle.mainBundle.bundleIdentifier ?: @"org.rdmachine";
    NSString* path = [[caches URLByAppendingPathComponent:bundle] URLByAppendingPathComponent:@"files"].path;
    // What an earlier run left (it ended without cleaning up).
    [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    return Std(path);
  }();
  return dir;
}

}  // namespace rdm
