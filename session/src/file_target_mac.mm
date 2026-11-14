// The app's side of the File Provider extension (fileprovider/): adds the
// domain, connects to the extension's service and translates between
// core's FileTarget (the clipboard's thread) and the extension (XPC).
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
  ~MacFileTarget() override;
  void SetRequester(FileRequester* requester) override;
  std::optional<std::vector<std::string>> Publish(uint64_t announce, const std::vector<FileEntry>& items) override;
  void Withdraw(uint64_t announce) override;
  void OnList(uint64_t announce, uint32_t item, const std::vector<FileEntry>& entries, const std::string& error) override;
  void OnFileStarted(uint64_t announce, uint32_t item, const FileEntry& entry, const std::string& local_path) override;
  void OnProgress(uint64_t announce, uint32_t item, const std::string& path, uint64_t bytes) override;
  void OnFileReady(uint64_t announce, uint32_t item, const FileEntry& entry, const std::string& local_path) override;
  void OnItemDone(uint64_t announce, uint32_t item, const std::string& error) override;

  FileRequester* requester = nullptr;  // under the hub's lock
};

}  // namespace rdm

namespace {

struct Hub {
  std::mutex mu;
  NSXPCConnection* conn = nil;
  NSURL* root = nil;
  NSFileProviderDomain* domain = nil;
  // What is on offer.
  std::string copy_id;
  rdm::MacFileTarget* owner = nullptr;
  uint64_t announce = 0;
  NSData* published = nil;
  std::set<std::string> requested;            // files the extension waits for
  std::map<std::string, std::string> ready;   // files that arrived first
  // Files on their way: where they are written and how much is there.
  std::map<std::string, std::pair<std::string, uint64_t>> growing;
  uint64_t counter = 0;
  // After a failed connection (RDMachine turned off in System Settings), no
  // new try for a while: each try may wait seconds on the clipboard's thread.
  NSDate* retry_after = nil;
  std::string last_error;
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
  if (idx.empty() || idx.find_first_not_of("0123456789") != std::string::npos) return false;
  *copy_id = id.substr(0, a);
  *item = static_cast<uint32_t>(std::stoul(idx));
  return true;
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
    hh.requested.clear();
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
  if (!ParseItemId(Std(topItemID), &copy_id, &item) || copy_id != h.copy_id || !h.owner || !h.owner->requester) {
    if (h.conn) [Proxy(h.conn) setTree:[NSJSONSerialization dataWithJSONObject:@{@"item" : topItemID, @"error" : @"gone"} options:0 error:nil]];
    return;
  }
  h.owner->requester->RequestFileList(h.announce, item);
}

- (void)fetchItem:(NSString*)itemID {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  const std::string id = Std(itemID);
  std::string copy_id;
  uint32_t item = 0;
  if (!ParseItemId(id, &copy_id, &item) || copy_id != h.copy_id || !h.owner || !h.owner->requester) {
    if (h.conn) [Proxy(h.conn) itemFailed:itemID reason:@"no longer on offer"];
    return;
  }
  if (auto it = h.ready.find(id); it != h.ready.end()) {
    Deliver(h, id, it->second);
    return;
  }
  if (auto it = h.growing.find(id); it != h.growing.end()) DeliverGrowing(h, id, it->second.first, it->second.second);
  h.requested.insert(id);
  h.owner->requester->RequestFileFetch(h.announce, item);
}
@end

namespace rdm {

MacFileTarget::~MacFileTarget() {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  if (h.owner == this) h.owner = nullptr;
}

void MacFileTarget::SetRequester(FileRequester* r) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  requester = r;
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
    h.copy_id = copy_id;
    h.owner = this;
    h.announce = announce;
    h.published = json;
    h.requested.clear();
    h.ready.clear();
    h.growing.clear();
    std::vector<std::string> paths;
    const std::string root = Std(h.root.path);
    for (const auto& e : items) paths.push_back(root + "/" + copy_id + "/" + e.path);
    // The copy's folder shows its items on disk only once it is listed
    // (until then they wait for their parent): list it now, so they are
    // there before anything pastes.
    NSString* folder = NS(root + "/" + copy_id);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      for (int i = 0; i < 50; ++i) {
        NSArray* names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:nil];
        if (names.count > 0) return;
        usleep(100000);
      }
    });
    return paths;
  }
}

void MacFileTarget::Withdraw(uint64_t announce) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  if (h.owner != this || h.announce != announce) return;
  if (h.conn) [Proxy(h.conn) withdrawCopy:NS(h.copy_id)];
  for (const auto& id : h.requested) {
    if (h.conn) [Proxy(h.conn) itemFailed:NS(id) reason:@"no longer on offer"];
  }
  h.owner = nullptr;
  h.copy_id.clear();
  h.published = nil;
  h.requested.clear();
  h.ready.clear();
  h.growing.clear();
}

void MacFileTarget::OnList(uint64_t announce, uint32_t item, const std::vector<FileEntry>& entries,
                           const std::string& error) {
  @autoreleasepool {
    Hub& h = TheHub();
    std::lock_guard lock(h.mu);
    if (h.owner != this || h.announce != announce || !h.conn) return;
    NSMutableDictionary* d = [NSMutableDictionary dictionary];
    d[@"item"] = NS(ItemId(h.copy_id, item));
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
    if (h.owner != this || h.announce != announce) return;
    const std::string id = ItemId(h.copy_id, item, entry.path);
    h.growing[id] = {local_path, 0};
    if (h.requested.count(id) && h.conn) DeliverGrowing(h, id, local_path, 0);
  }
}

void MacFileTarget::OnProgress(uint64_t announce, uint32_t item, const std::string& path, uint64_t bytes) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  if (h.owner != this || h.announce != announce) return;
  const std::string id = ItemId(h.copy_id, item, path);
  if (auto it = h.growing.find(id); it != h.growing.end()) it->second.second = bytes;
  if (h.requested.count(id) && h.conn) [Proxy(h.conn) progress:NS(id) bytes:bytes];
}

void MacFileTarget::OnFileReady(uint64_t announce, uint32_t item, const FileEntry& entry, const std::string& local_path) {
  if (entry.kind != FileKind::kFile) return;  // folders and links are metadata only
  @autoreleasepool {
    Hub& h = TheHub();
    std::lock_guard lock(h.mu);
    if (h.owner != this || h.announce != announce) return;
    const std::string id = ItemId(h.copy_id, item, entry.path);
    h.growing.erase(id);
    if (h.requested.erase(id) && h.conn) {
      Deliver(h, id, local_path);
    } else {
      h.ready[id] = local_path;
    }
  }
}

void MacFileTarget::OnItemDone(uint64_t announce, uint32_t item, const std::string& error) {
  Hub& h = TheHub();
  std::lock_guard lock(h.mu);
  if (h.owner != this || h.announce != announce) return;
  const std::string prefix = ItemId(h.copy_id, item);
  for (auto it = h.requested.begin(); it != h.requested.end();) {
    if (*it == prefix || it->rfind(prefix + "/", 0) == 0) {
      if (h.conn) [Proxy(h.conn) itemFailed:NS(*it) reason:NS(error.empty() ? "not in what the peer sent" : error)];
      it = h.requested.erase(it);
    } else {
      ++it;
    }
  }
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

std::shared_ptr<FileTarget> SystemFileTarget() {
  static const bool has = HasExtension();
  if (!has) return nullptr;
  return std::make_shared<MacFileTarget>();
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
