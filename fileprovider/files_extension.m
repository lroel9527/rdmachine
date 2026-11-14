// The File Provider extension inside RDMachine.app and RDMachine Server.app
// (docs/file-paste.md): the peer's copied files and folders as placeholders
// (names, sizes, no content) that the clipboard points at. When Finder
// copies one, the system asks for its content here and this asks the app,
// which fetches it from the peer and hands the file over.
//
// Sandboxed, no network: everything comes from the app over XPC
// (rdm_files_xpc.h). It keeps only the current copy, in memory; when it is
// started again the app publishes again.
#import <FileProvider/FileProvider.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <os/log.h>
#include <fcntl.h>
#include <sys/clonefile.h>
#include <unistd.h>

#import "rdm_files_xpc.h"

static os_log_t Log(void) {
  static os_log_t log;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ log = os_log_create("org.rdmachine.files", "extension"); });
  return log;
}

// ---- items -------------------------------------------------------------------

@interface RDMItem : NSObject <NSFileProviderItem>
@property(nonatomic, copy) NSString* identifier;
@property(nonatomic, copy) NSString* parent;
@property(nonatomic, copy) NSString* name;
@property(nonatomic) char kind;  // 'f' file, 'd' folder, 'l' link
@property(nonatomic) unsigned long long size;
@property(nonatomic) unsigned mode;
@property(nonatomic) long long mtime;
@property(nonatomic, copy, nullable) NSString* link;
@property(nonatomic, copy) NSString* version;
@end

@implementation RDMItem
- (NSFileProviderItemIdentifier)itemIdentifier { return self.identifier; }
- (NSFileProviderItemIdentifier)parentItemIdentifier { return self.parent; }
- (NSString*)filename { return self.name; }
- (UTType*)contentType {
  if (self.kind == 'd') return UTTypeFolder;
  if (self.kind == 'l') return UTTypeSymbolicLink;
  UTType* t = self.name.pathExtension.length ? [UTType typeWithFilenameExtension:self.name.pathExtension] : nil;
  return t ?: UTTypeData;
}
- (NSNumber*)documentSize { return self.kind == 'f' ? @(self.size) : nil; }
- (NSFileProviderItemCapabilities)capabilities {
  // Items that cannot be written are locked (uchg), and Finder copies the
  // lock out with them; changes are refused when they come (modifyItem).
  NSFileProviderItemCapabilities c = NSFileProviderItemCapabilitiesAllowsReading |
                                     NSFileProviderItemCapabilitiesAllowsWriting |
                                     NSFileProviderItemCapabilitiesAllowsRenaming |
                                     NSFileProviderItemCapabilitiesAllowsReparenting |
                                     NSFileProviderItemCapabilitiesAllowsDeleting;
  if (self.kind == 'd') {
    c |= NSFileProviderItemCapabilitiesAllowsContentEnumerating | NSFileProviderItemCapabilitiesAllowsAddingSubItems;
  }
  return c;
}
- (NSFileProviderFileSystemFlags)fileSystemFlags {
  // Writable, so what Finder copies out is an ordinary file (a read-only
  // item comes out locked).
  NSFileProviderFileSystemFlags f = NSFileProviderFileSystemUserReadable | NSFileProviderFileSystemUserWritable;
  if (self.kind == 'd' || (self.mode & 0100)) f |= NSFileProviderFileSystemUserExecutable;
  return f;
}
- (NSFileProviderItemVersion*)itemVersion {
  NSData* v = [self.version dataUsingEncoding:NSUTF8StringEncoding];
  return [[NSFileProviderItemVersion alloc] initWithContentVersion:v metadataVersion:v];
}
- (NSDate*)contentModificationDate { return self.mtime ? [NSDate dateWithTimeIntervalSince1970:self.mtime] : nil; }
- (NSDate*)creationDate { return self.contentModificationDate; }
- (NSString*)symlinkTargetPath { return self.kind == 'l' ? self.link : nil; }
@end

// ---- the model ---------------------------------------------------------------

@interface RDMFetch : NSObject
@property(nonatomic, copy) void (^completion)(NSURL*, id<NSFileProviderItem>, NSError*);
@property(nonatomic, strong) NSProgress* progress;
@end
@implementation RDMFetch
@end

@class RDMModel;

@interface RDMService : NSObject <RDMFilesExtension, NSXPCListenerDelegate>
@end

// Everything the extension knows, on one serial queue. One per process.
@interface RDMModel : NSObject
@property(nonatomic, readonly) dispatch_queue_t queue;
@property(nonatomic, strong, nullable) NSFileProviderDomain* domain;
@property(nonatomic, strong, nullable) NSXPCConnection* host;
@property(nonatomic, copy, nullable) NSString* currentCopy;
@property(nonatomic, strong) NSMutableDictionary<NSString*, RDMItem*>* items;
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSMutableArray<NSString*>*>* children;
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSMutableArray*>* waitingTree;  // top item -> blocks
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSMutableArray<RDMFetch*>*>* fetches;
@property(nonatomic, strong) NSMutableArray<NSString*>* deleted;
@property(nonatomic, strong) NSMutableArray<NSNumber*>* deletedAt;
@property(nonatomic) unsigned long long generation;
@property(nonatomic, copy) NSString* epoch;  // anchors of an earlier process are not ours
@property(nonatomic, strong) RDMService* service;
+ (instancetype)shared;
@end

@implementation RDMModel
+ (instancetype)shared {
  static RDMModel* m;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ m = [[RDMModel alloc] init]; });
  return m;
}
- (instancetype)init {
  if (!(self = [super init])) return nil;
  _queue = dispatch_queue_create("org.rdmachine.files.model", DISPATCH_QUEUE_SERIAL);
  _items = [NSMutableDictionary dictionary];
  _children = [NSMutableDictionary dictionary];
  _waitingTree = [NSMutableDictionary dictionary];
  _fetches = [NSMutableDictionary dictionary];
  _deleted = [NSMutableArray array];
  _deletedAt = [NSMutableArray array];
  _epoch = [NSUUID UUID].UUIDString;
  _children[NSFileProviderRootContainerItemIdentifier] = [NSMutableArray array];
  _service = [[RDMService alloc] init];
  return self;
}

- (NSFileProviderSyncAnchor)anchor {
  return [[NSString stringWithFormat:@"%@:%llu", self.epoch, self.generation] dataUsingEncoding:NSUTF8StringEncoding];
}

// The generation an anchor of ours stands for; -1 if it is not ours.
- (long long)generationOfAnchor:(NSFileProviderSyncAnchor)anchor {
  NSString* s = [[NSString alloc] initWithData:anchor encoding:NSUTF8StringEncoding];
  NSArray* parts = [s componentsSeparatedByString:@":"];
  if (parts.count != 2 || ![parts[0] isEqualToString:self.epoch]) return -1;
  return [parts[1] longLongValue];
}

- (void)signal {
  NSFileProviderDomain* d = self.domain;
  if (!d) return;
  [[NSFileProviderManager managerForDomain:d]
      signalEnumeratorForContainerItemIdentifier:NSFileProviderWorkingSetContainerItemIdentifier
                               completionHandler:^(NSError* e) {
                                 if (e) os_log_error(Log(), "signal: %{public}@", e);
                               }];
}

// Removes the current copy (recorded as a deletion of its folder).
- (void)dropCopy {
  if (!self.currentCopy) return;
  self.generation++;
  [self.deleted addObject:self.currentCopy];
  [self.deletedAt addObject:@(self.generation)];
  NSError* gone = [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil];
  for (NSString* key in self.fetches) {
    for (RDMFetch* f in self.fetches[key]) f.completion(nil, nil, gone);
  }
  [self.fetches removeAllObjects];
  for (NSString* key in self.waitingTree) {
    for (void (^b)(NSString*) in self.waitingTree[key]) b(@"gone");
  }
  [self.waitingTree removeAllObjects];
  [self.items removeAllObjects];
  [self.children removeAllObjects];
  self.children[NSFileProviderRootContainerItemIdentifier] = [NSMutableArray array];
  self.currentCopy = nil;
}

- (RDMItem*)itemFromEntry:(NSDictionary*)e identifier:(NSString*)ident parent:(NSString*)parent name:(NSString*)name {
  RDMItem* it = [[RDMItem alloc] init];
  it.identifier = ident;
  it.parent = parent;
  it.name = name;
  NSString* k = e[@"k"];
  it.kind = [k isEqualToString:@"d"] ? 'd' : [k isEqualToString:@"l"] ? 'l' : 'f';
  it.size = [e[@"s"] unsignedLongLongValue];
  it.mode = [e[@"m"] unsignedIntValue];
  it.mtime = [e[@"t"] longLongValue];
  it.link = e[@"l"];
  it.version = self.currentCopy ?: @"";
  return it;
}

// Top item identifier ("c3/0") of any item inside a copy, or nil.
+ (NSString*)topOf:(NSString*)ident {
  NSArray* parts = [ident componentsSeparatedByString:@"/"];
  if (parts.count < 2) return nil;
  return [NSString stringWithFormat:@"%@/%@", parts[0], parts[1]];
}
@end

// ---- the service the app connects to ---------------------------------------------

@implementation RDMService

- (BOOL)listener:(NSXPCListener*)listener shouldAcceptNewConnection:(NSXPCConnection*)c {
  c.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(RDMFilesExtension)];
  c.exportedObject = self;
  c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(RDMFilesHost)];
  RDMModel* m = [RDMModel shared];
  __weak NSXPCConnection* weak = c;
  c.invalidationHandler = ^{
    dispatch_async(m.queue, ^{
      if (m.host == weak) m.host = nil;
    });
  };
  dispatch_sync(m.queue, ^{ m.host = c; });
  [c resume];
  os_log(Log(), "the app connected");
  return YES;
}

- (void)publish:(NSData*)json reply:(void (^)(NSString*))reply {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    NSDictionary* d = [NSJSONSerialization JSONObjectWithData:json options:0 error:nil];
    NSString* copyID = [d isKindOfClass:NSDictionary.class] ? d[@"copy"] : nil;
    NSArray* entries = [d isKindOfClass:NSDictionary.class] ? d[@"items"] : nil;
    if (![copyID isKindOfClass:NSString.class] || ![entries isKindOfClass:NSArray.class]) {
      reply(@"malformed");
      return;
    }
    [m dropCopy];
    m.currentCopy = copyID;
    m.generation++;
    RDMItem* folder = [[RDMItem alloc] init];
    folder.identifier = copyID;
    folder.parent = NSFileProviderRootContainerItemIdentifier;
    folder.name = copyID;
    folder.kind = 'd';
    folder.mode = 0755;
    folder.mtime = (long long)[NSDate date].timeIntervalSince1970;
    folder.version = copyID;
    m.items[copyID] = folder;
    m.children[NSFileProviderRootContainerItemIdentifier] = [NSMutableArray arrayWithObject:copyID];
    NSMutableArray* tops = [NSMutableArray array];
    for (NSUInteger i = 0; i < entries.count; i++) {
      NSDictionary* e = entries[i];
      if (![e isKindOfClass:NSDictionary.class] || ![e[@"p"] isKindOfClass:NSString.class]) continue;
      NSString* ident = [NSString stringWithFormat:@"%@/%lu", copyID, (unsigned long)i];
      m.items[ident] = [m itemFromEntry:e identifier:ident parent:copyID name:e[@"p"]];
      [tops addObject:ident];
    }
    m.children[copyID] = tops;
    os_log(Log(), "published %{public}@ with %lu items", copyID, (unsigned long)tops.count);
    [m signal];
    reply(nil);
  });
}

- (void)withdrawCopy:(NSString*)copyID {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    if (![m.currentCopy isEqualToString:copyID]) return;
    [m dropCopy];
    [m signal];
  });
}

- (void)setTree:(NSData*)json {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    NSDictionary* d = [NSJSONSerialization JSONObjectWithData:json options:0 error:nil];
    NSString* top = [d isKindOfClass:NSDictionary.class] ? d[@"item"] : nil;
    if (![top isKindOfClass:NSString.class]) return;
    NSArray* waiting = m.waitingTree[top];
    [m.waitingTree removeObjectForKey:top];
    NSString* error = d[@"error"];
    NSArray* entries = d[@"entries"];
    RDMItem* topItem = m.items[top];
    if (!topItem || [error isKindOfClass:NSString.class] || ![entries isKindOfClass:NSArray.class]) {
      for (void (^b)(NSString*) in waiting) b([error isKindOfClass:NSString.class] ? error : @"gone");
      return;
    }
    if (!m.children[top]) m.children[top] = [NSMutableArray array];
    for (NSDictionary* e in entries) {
      NSString* p = [e isKindOfClass:NSDictionary.class] ? e[@"p"] : nil;
      if (![p isKindOfClass:NSString.class] || p.length == 0) continue;  // the item itself
      NSString* ident = [NSString stringWithFormat:@"%@/%@", top, p];
      NSString* dir = p.stringByDeletingLastPathComponent;
      NSString* parent = dir.length ? [NSString stringWithFormat:@"%@/%@", top, dir] : top;
      RDMItem* it = [m itemFromEntry:e identifier:ident parent:parent name:p.lastPathComponent];
      m.items[ident] = it;
      if (!m.children[parent]) m.children[parent] = [NSMutableArray array];
      [m.children[parent] addObject:ident];
      if (it.kind == 'd' && !m.children[ident]) m.children[ident] = [NSMutableArray array];
    }
    os_log(Log(), "tree of %{public}@: %lu entries", top, (unsigned long)entries.count);
    for (void (^b)(NSString*) in waiting) b(nil);
  });
}

// Hands a fetched file to the system: cloned (or copied) into the domain's
// temporary folder, which the system takes it from.
- (void)fileReady:(NSString*)itemID handle:(NSFileHandle*)handle {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    NSArray<RDMFetch*>* waiting = m.fetches[itemID];
    [m.fetches removeObjectForKey:itemID];
    RDMItem* item = m.items[itemID];
    if (!waiting.count || !item || !m.domain) return;
    NSError* error = nil;
    NSURL* tmp = [[NSFileProviderManager managerForDomain:m.domain] temporaryDirectoryURLWithError:&error];
    NSURL* dst = [tmp URLByAppendingPathComponent:[NSUUID UUID].UUIDString];
    BOOL ok = NO;
    if (dst) {
      const int src = handle.fileDescriptor;
      ok = fclonefileat(src, AT_FDCWD, dst.fileSystemRepresentation, 0) == 0;
      if (!ok) {
        // Not on the same volume: copy.
        const int out = open(dst.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0) {
          lseek(src, 0, SEEK_SET);
          char buf[1 << 16];
          ssize_t n;
          ok = YES;
          while ((n = read(src, buf, sizeof buf)) > 0) {
            if (write(out, buf, (size_t)n) != n) {
              ok = NO;
              break;
            }
          }
          if (n < 0) ok = NO;
          close(out);
        }
      }
    }
    [handle closeFile];
    if (!ok) {
      os_log_error(Log(), "could not take %{public}@: %{public}@", itemID, error);
      NSError* e = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil];
      for (RDMFetch* f in waiting) f.completion(nil, nil, e);
      return;
    }
    // Every request of the item gets the file (the first takes it, the
    // others get their own copies).
    for (NSUInteger i = 0; i < waiting.count; i++) {
      NSURL* u = dst;
      if (i + 1 < waiting.count) {
        u = [tmp URLByAppendingPathComponent:[NSUUID UUID].UUIDString];
        if (clonefile(dst.fileSystemRepresentation, u.fileSystemRepresentation, 0) != 0) u = nil;
      }
      waiting[i].progress.completedUnitCount = waiting[i].progress.totalUnitCount;
      waiting[i].completion(u, item, u ? nil : [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil]);
    }
  });
}

- (void)progress:(NSString*)itemID bytes:(unsigned long long)bytes {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    for (RDMFetch* f in m.fetches[itemID]) f.progress.completedUnitCount = (int64_t)bytes;
  });
}

- (void)itemFailed:(NSString*)itemID reason:(NSString*)reason {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    NSArray<RDMFetch*>* waiting = m.fetches[itemID];
    [m.fetches removeObjectForKey:itemID];
    os_log_error(Log(), "%{public}@ failed: %{public}@", itemID, reason);
    NSError* e = [NSError errorWithDomain:NSFileProviderErrorDomain
                                     code:NSFileProviderErrorServerUnreachable
                                 userInfo:@{NSLocalizedDescriptionKey : reason}];
    for (RDMFetch* f in waiting) f.completion(nil, nil, e);
  });
}

@end

@interface RDMServiceSource : NSObject <NSFileProviderServiceSource>
@property(nonatomic, strong) NSXPCListener* listener;
@end

@implementation RDMServiceSource
- (NSFileProviderServiceName)serviceName { return RDMFilesServiceName; }
- (NSXPCListenerEndpoint*)makeListenerEndpointAndReturnError:(NSError**)error {
  if (!self.listener) {
    self.listener = [NSXPCListener anonymousListener];
    self.listener.delegate = [RDMModel shared].service;
    [self.listener resume];
  }
  return self.listener.endpoint;
}
@end

// ---- enumeration ---------------------------------------------------------------

@interface RDMEnumerator : NSObject <NSFileProviderEnumerator>
@property(nonatomic, copy) NSString* container;
@end

@implementation RDMEnumerator
- (void)invalidate {}

- (void)enumerateItemsForObserver:(id<NSFileProviderEnumerationObserver>)observer
                   startingAtPage:(NSFileProviderPage)page {
  RDMModel* m = [RDMModel shared];
  NSString* container = self.container;
  dispatch_async(m.queue, ^{
    if ([container isEqualToString:NSFileProviderWorkingSetContainerItemIdentifier]) {
      [observer didEnumerateItems:m.items.allValues];
      [observer finishEnumeratingUpToPage:nil];
      return;
    }
    NSArray* kids = m.children[container];
    if (kids) {
      NSMutableArray* items = [NSMutableArray array];
      for (NSString* k in kids) {
        if (m.items[k]) [items addObject:m.items[k]];
      }
      [observer didEnumerateItems:items];
      [observer finishEnumeratingUpToPage:nil];
      return;
    }
    RDMItem* item = m.items[container];
    NSString* top = [RDMModel topOf:container];
    if (!item || item.kind != 'd' || !top) {
      [observer finishEnumeratingWithError:[NSError errorWithDomain:NSFileProviderErrorDomain
                                                               code:NSFileProviderErrorNoSuchItem
                                                           userInfo:nil]];
      return;
    }
    // A folder of the peer's whose contents are not known yet: ask the app.
    void (^answer)(NSString*) = ^(NSString* error) {
      // On the model's queue.
      if (error) {
        [observer finishEnumeratingWithError:[NSError errorWithDomain:NSFileProviderErrorDomain
                                                                 code:NSFileProviderErrorServerUnreachable
                                                             userInfo:@{NSLocalizedDescriptionKey : error}]];
        return;
      }
      NSMutableArray* items = [NSMutableArray array];
      for (NSString* k in m.children[container] ?: @[]) {
        if (m.items[k]) [items addObject:m.items[k]];
      }
      [observer didEnumerateItems:items];
      [observer finishEnumeratingUpToPage:nil];
    };
    BOOL asked = m.waitingTree[top] != nil;
    if (!asked) m.waitingTree[top] = [NSMutableArray array];
    [m.waitingTree[top] addObject:answer];
    if (asked) return;
    NSXPCConnection* host = m.host;
    if (!host) {
      NSArray* w = m.waitingTree[top];
      [m.waitingTree removeObjectForKey:top];
      for (void (^b)(NSString*) in w) b(@"RDMachine is not connected");
      return;
    }
    [[host remoteObjectProxyWithErrorHandler:^(NSError* e) {
      (void)e;
      dispatch_async(m.queue, ^{
        NSArray* w = m.waitingTree[top];
        [m.waitingTree removeObjectForKey:top];
        for (void (^b)(NSString*) in w) b(@"RDMachine is not connected");
      });
    }] listItem:top];
  });
}

- (void)enumerateChangesForObserver:(id<NSFileProviderChangeObserver>)observer
                     fromSyncAnchor:(NSFileProviderSyncAnchor)anchor {
  RDMModel* m = [RDMModel shared];
  NSString* container = self.container;
  dispatch_async(m.queue, ^{
    const long long since = [m generationOfAnchor:anchor];
    if (since < 0) {
      [observer finishEnumeratingWithError:[NSError errorWithDomain:NSFileProviderErrorDomain
                                                               code:NSFileProviderErrorSyncAnchorExpired
                                                           userInfo:nil]];
      return;
    }
    if ([container isEqualToString:NSFileProviderWorkingSetContainerItemIdentifier] &&
        (unsigned long long)since < m.generation) {
      NSMutableArray* gone = [NSMutableArray array];
      for (NSUInteger i = 0; i < m.deleted.count; i++) {
        if (m.deletedAt[i].longLongValue > since) [gone addObject:m.deleted[i]];
      }
      if (gone.count) [observer didDeleteItemsWithIdentifiers:gone];
      [observer didUpdateItems:m.items.allValues];
    }
    [observer finishEnumeratingChangesUpToSyncAnchor:[m anchor] moreComing:NO];
  });
}

- (void)currentSyncAnchorWithCompletionHandler:(void (^)(NSFileProviderSyncAnchor))completionHandler {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{ completionHandler([m anchor]); });
}
@end

// ---- the extension -----------------------------------------------------------------

@interface FilesExtension : NSObject <NSFileProviderReplicatedExtension, NSFileProviderServicing>
@end

@implementation FilesExtension

- (instancetype)initWithDomain:(NSFileProviderDomain*)domain {
  if (!(self = [super init])) return nil;
  RDMModel* m = [RDMModel shared];
  dispatch_sync(m.queue, ^{ m.domain = domain; });
  os_log(Log(), "started for domain %{public}@", domain.identifier);
  return self;
}

- (void)invalidate {}

- (NSProgress*)itemForIdentifier:(NSFileProviderItemIdentifier)identifier
                         request:(NSFileProviderRequest*)request
               completionHandler:(void (^)(NSFileProviderItem, NSError*))completionHandler {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    if ([identifier isEqualToString:NSFileProviderRootContainerItemIdentifier]) {
      RDMItem* root = [[RDMItem alloc] init];
      root.identifier = NSFileProviderRootContainerItemIdentifier;
      root.parent = NSFileProviderRootContainerItemIdentifier;
      root.name = @"RDMachine";
      root.kind = 'd';
      root.mode = 0755;
      root.version = @"root";
      completionHandler(root, nil);
      return;
    }
    RDMItem* it = m.items[identifier];
    completionHandler(it, it ? nil
                             : [NSError errorWithDomain:NSFileProviderErrorDomain
                                                   code:NSFileProviderErrorNoSuchItem
                                               userInfo:nil]);
  });
  return [NSProgress progressWithTotalUnitCount:1];
}

- (NSProgress*)fetchContentsForItemWithIdentifier:(NSFileProviderItemIdentifier)itemIdentifier
                                          version:(NSFileProviderItemVersion*)requestedVersion
                                          request:(NSFileProviderRequest*)request
                                completionHandler:(void (^)(NSURL*, NSFileProviderItem, NSError*))completionHandler {
  RDMModel* m = [RDMModel shared];
  NSProgress* progress = [NSProgress progressWithTotalUnitCount:1];
  RDMFetch* fetch = [[RDMFetch alloc] init];
  fetch.progress = progress;
  __block BOOL completed = NO;
  fetch.completion = ^(NSURL* u, id<NSFileProviderItem> item, NSError* e) {
    // On the model's queue; only the first answer counts.
    if (completed) return;
    completed = YES;
    completionHandler(u, item, e);
  };
  progress.cancellationHandler = ^{
    dispatch_async(m.queue, ^{
      [m.fetches[itemIdentifier] removeObject:fetch];
      fetch.completion(nil, nil, [NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
    });
  };
  dispatch_async(m.queue, ^{
    RDMItem* item = m.items[itemIdentifier];
    if (!item || item.kind != 'f') {
      fetch.completion(nil, nil, [NSError errorWithDomain:NSFileProviderErrorDomain
                                                     code:NSFileProviderErrorNoSuchItem
                                                 userInfo:nil]);
      return;
    }
    progress.totalUnitCount = (int64_t)MAX(item.size, 1ULL);
    NSXPCConnection* host = m.host;
    if (!host) {
      fetch.completion(nil, nil, [NSError errorWithDomain:NSFileProviderErrorDomain
                                                     code:NSFileProviderErrorServerUnreachable
                                                 userInfo:nil]);
      return;
    }
    if (!m.fetches[itemIdentifier]) m.fetches[itemIdentifier] = [NSMutableArray array];
    [m.fetches[itemIdentifier] addObject:fetch];
    os_log(Log(), "fetch %{public}@", itemIdentifier);
    [[host remoteObjectProxyWithErrorHandler:^(NSError* e) {
      (void)e;
      dispatch_async(m.queue, ^{
        [m.fetches[itemIdentifier] removeObject:fetch];
        fetch.completion(nil, nil, [NSError errorWithDomain:NSFileProviderErrorDomain
                                                       code:NSFileProviderErrorServerUnreachable
                                                   userInfo:nil]);
      });
    }] fetchItem:itemIdentifier];
  });
  return progress;
}

// Read-only: nothing is created, changed or deleted from this side.
- (NSProgress*)createItemBasedOnTemplate:(NSFileProviderItem)itemTemplate
                                  fields:(NSFileProviderItemFields)fields
                                contents:(NSURL*)url
                                 options:(NSFileProviderCreateItemOptions)options
                                 request:(NSFileProviderRequest*)request
                       completionHandler:(void (^)(NSFileProviderItem, NSFileProviderItemFields, BOOL, NSError*))completionHandler {
  completionHandler(nil, 0, NO, [NSError errorWithDomain:NSCocoaErrorDomain code:NSFeatureUnsupportedError userInfo:nil]);
  return [NSProgress progressWithTotalUnitCount:1];
}

- (NSProgress*)modifyItem:(NSFileProviderItem)item
              baseVersion:(NSFileProviderItemVersion*)version
            changedFields:(NSFileProviderItemFields)changedFields
                 contents:(NSURL*)newContents
                  options:(NSFileProviderModifyItemOptions)options
                  request:(NSFileProviderRequest*)request
        completionHandler:(void (^)(NSFileProviderItem, NSFileProviderItemFields, BOOL, NSError*))completionHandler {
  completionHandler(nil, 0, NO, [NSError errorWithDomain:NSCocoaErrorDomain code:NSFeatureUnsupportedError userInfo:nil]);
  return [NSProgress progressWithTotalUnitCount:1];
}

- (NSProgress*)deleteItemWithIdentifier:(NSFileProviderItemIdentifier)identifier
                            baseVersion:(NSFileProviderItemVersion*)version
                                options:(NSFileProviderDeleteItemOptions)options
                                request:(NSFileProviderRequest*)request
                      completionHandler:(void (^)(NSError*))completionHandler {
  completionHandler([NSError errorWithDomain:NSCocoaErrorDomain code:NSFeatureUnsupportedError userInfo:nil]);
  return [NSProgress progressWithTotalUnitCount:1];
}

- (id<NSFileProviderEnumerator>)enumeratorForContainerItemIdentifier:(NSFileProviderItemIdentifier)containerItemIdentifier
                                                             request:(NSFileProviderRequest*)request
                                                               error:(NSError**)error {
  RDMEnumerator* e = [[RDMEnumerator alloc] init];
  e.container = containerItemIdentifier;
  return e;
}

- (NSProgress*)supportedServiceSourcesForItemIdentifier:(NSFileProviderItemIdentifier)itemIdentifier
                                      completionHandler:(void (^)(NSArray<id<NSFileProviderServiceSource>>*, NSError*))completionHandler {
  static RDMServiceSource* source;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ source = [[RDMServiceSource alloc] init]; });
  completionHandler(@[ source ], nil);
  return [NSProgress progressWithTotalUnitCount:1];
}

@end
