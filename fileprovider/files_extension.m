// The File Provider extension inside RDMachine.app and RDMachine Server.app
// (docs/file-paste.md): the peer's copied files and folders as placeholders
// (names, sizes, no content) that the clipboard points at. When Finder
// copies one, the system asks for its content here and this asks the app,
// which fetches it from the peer and hands the file over.
//
// Sandboxed, no network: everything comes from the app over XPC
// (rdm_files_xpc.h). It keeps the copies the app published, in memory, each
// in a folder of its own, until the app withdraws them (a copy stays while
// Finder copies from it, also when a newer one came); when it is started
// again the app publishes them again.
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
// Finder tags and the like: the system puts them on the placeholder and on
// what is copied from it.
@property(nonatomic, strong, nullable) NSDictionary<NSString*, NSData*>* extendedAttributes;
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

// A range of a file the system asked for (fetchPartialContents).
@interface RDMPartial : NSObject
@property(nonatomic) NSRange range;
@property(nonatomic) unsigned long long alignment;
@property(nonatomic, copy) void (^completion)(NSURL*, id<NSFileProviderItem>, NSRange, NSFileProviderMaterializationFlags,
                                              NSError*);
@property(nonatomic, strong) NSProgress* progress;
@end
@implementation RDMPartial
@end

// A file on its way: what reads it, and how much of it is there.
@interface RDMGrowing : NSObject
@property(nonatomic, strong) NSFileHandle* handle;
@property(nonatomic) unsigned long long available;
@end
@implementation RDMGrowing
@end

@class RDMModel;

@interface RDMService : NSObject <RDMFilesExtension, NSXPCListenerDelegate>
@end

// Everything the extension knows, on one serial queue. One per process.
@interface RDMModel : NSObject
@property(nonatomic, readonly) dispatch_queue_t queue;
@property(nonatomic, strong, nullable) NSFileProviderDomain* domain;
@property(nonatomic, strong, nullable) NSXPCConnection* host;
@property(nonatomic, strong) NSMutableArray<NSString*>* publishedCopies;  // oldest first
// Which app connection published each copy: when that app goes away (it
// quit, crashed or restarted), so do its copies; nothing could fetch them.
@property(nonatomic, strong) NSMapTable<NSString*, NSXPCConnection*>* publishers;
@property(nonatomic, strong) NSMutableDictionary<NSString*, RDMItem*>* items;
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSMutableArray<NSString*>*>* children;
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSMutableArray*>* waitingTree;  // top item -> blocks
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSMutableArray<RDMFetch*>*>* fetches;
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSMutableArray<RDMPartial*>*>* partials;
@property(nonatomic, strong) NSMutableDictionary<NSString*, RDMGrowing*>* growing;
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
  _publishedCopies = [NSMutableArray array];
  _publishers = [NSMapTable strongToWeakObjectsMapTable];
  _children = [NSMutableDictionary dictionary];
  _waitingTree = [NSMutableDictionary dictionary];
  _fetches = [NSMutableDictionary dictionary];
  _partials = [NSMutableDictionary dictionary];
  _growing = [NSMutableDictionary dictionary];
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

// Whether `ident` is `copyID`'s folder or anything in it.
static BOOL InCopy(NSString* ident, NSString* copyID) {
  return [ident isEqualToString:copyID] || [ident hasPrefix:[copyID stringByAppendingString:@"/"]];
}

// Removes a copy (recorded as a deletion of its folder).
- (void)dropCopy:(NSString*)copyID {
  if (![self.publishedCopies containsObject:copyID]) return;
  [self.publishedCopies removeObject:copyID];
  self.generation++;
  [self.deleted addObject:copyID];
  [self.deletedAt addObject:@(self.generation)];
  NSError* gone = [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil];
  for (NSString* key in self.fetches.allKeys) {
    if (!InCopy(key, copyID)) continue;
    for (RDMFetch* f in self.fetches[key]) f.completion(nil, nil, gone);
    [self.fetches removeObjectForKey:key];
  }
  for (NSString* key in self.partials.allKeys) {
    if (!InCopy(key, copyID)) continue;
    for (RDMPartial* p in self.partials[key]) p.completion(nil, nil, NSMakeRange(0, 0), 0, gone);
    [self.partials removeObjectForKey:key];
  }
  for (NSString* key in self.growing.allKeys) {
    if (InCopy(key, copyID)) [self.growing removeObjectForKey:key];
  }
  for (NSString* key in self.waitingTree.allKeys) {
    if (!InCopy(key, copyID)) continue;
    for (void (^b)(NSString*) in self.waitingTree[key]) b(@"gone");
    [self.waitingTree removeObjectForKey:key];
  }
  for (NSString* key in self.items.allKeys) {
    if (InCopy(key, copyID)) [self.items removeObjectForKey:key];
  }
  for (NSString* key in self.children.allKeys) {
    if (InCopy(key, copyID)) [self.children removeObjectForKey:key];
  }
  [self.children[NSFileProviderRootContainerItemIdentifier] removeObject:copyID];
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
  NSArray* parts = [ident componentsSeparatedByString:@"/"];
  it.version = parts.firstObject ?: @"";
  NSDictionary* x = e[@"x"];
  if ([x isKindOfClass:NSDictionary.class] && x.count) {
    NSMutableDictionary<NSString*, NSData*>* attrs = [NSMutableDictionary dictionary];
    for (NSString* name in x) {
      NSString* v = x[name];
      NSData* d = [v isKindOfClass:NSString.class] ? [[NSData alloc] initWithBase64EncodedString:v options:0] : nil;
      if ([name isKindOfClass:NSString.class] && d) attrs[name] = d;
    }
    it.extendedAttributes = attrs;
  }
  return it;
}

// Hands on what has arrived of a file to the ranges waiting for it: each
// whose range is covered (or all, once the file is complete) gets a clone of
// the file as it is now, with everything that is there from its aligned
// start, so the reader (Finder copying) goes on at once. On the queue.
- (void)servePartials:(NSString*)itemID fd:(int)fd available:(unsigned long long)available complete:(BOOL)complete {
  NSMutableArray<RDMPartial*>* waiting = self.partials[itemID];
  RDMItem* item = self.items[itemID];
  if (!waiting.count || !item || !self.domain || fd < 0) return;
  NSURL* tmp = [[NSFileProviderManager managerForDomain:self.domain] temporaryDirectoryURLWithError:nil];
  const unsigned long long size = item.size;
  for (RDMPartial* p in [waiting copy]) {
    const unsigned long long align = MAX(p.alignment, 1ULL);
    const unsigned long long loc = p.range.location;
    const unsigned long long want_end = MIN(loc + p.range.length, size);
    unsigned long long end = complete ? size : (available / align) * align;
    if (end > size) end = size;
    if (available > loc) p.progress.completedUnitCount = (int64_t)MIN(available - loc, (unsigned long long)p.range.length);
    if (!complete && end < want_end) continue;
    const unsigned long long start = MIN((loc / align) * align, end);
    NSURL* dst = [tmp URLByAppendingPathComponent:[NSUUID UUID].UUIDString];
    BOOL ok = dst && fclonefileat(fd, AT_FDCWD, dst.fileSystemRepresentation, 0) == 0;
    if (!ok && dst) {
      // Not on the same volume: copy what is there.
      const int out = open(dst.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (out >= 0) {
        char buf[1 << 16];
        ok = YES;
        for (off_t at = 0; at < (off_t)end;) {
          const ssize_t n = pread(fd, buf, (size_t)MIN((off_t)sizeof buf, (off_t)end - at), at);
          if (n <= 0 || pwrite(out, buf, (size_t)n, at) != n) {
            ok = NO;
            break;
          }
          at += n;
        }
        close(out);
      }
    }
    [waiting removeObject:p];
    if (!ok) {
      p.completion(nil, nil, NSMakeRange(0, 0), 0,
                   [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil]);
      continue;
    }
    p.progress.completedUnitCount = p.progress.totalUnitCount;
    p.completion(dst, item, NSMakeRange((NSUInteger)start, (NSUInteger)(end - start)), 0, nil);
  }
  if (!waiting.count) [self.partials removeObjectForKey:itemID];
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
      // The app that published these is gone (a new process of it publishes
      // its own again).
      BOOL dropped = NO;
      for (NSString* copyID in [m.publishedCopies copy]) {
        NSXPCConnection* owner = [m.publishers objectForKey:copyID];
        if (owner && owner != weak) continue;
        [m dropCopy:copyID];
        [m.publishers removeObjectForKey:copyID];
        os_log(Log(), "dropped %{public}@: the app that published it is gone", copyID);
        dropped = YES;
      }
      if (dropped) [m signal];
    });
  };
  dispatch_sync(m.queue, ^{ m.host = c; });
  [c resume];
  os_log(Log(), "the app connected");
  return YES;
}

- (void)publish:(NSData*)json reply:(void (^)(NSString*))reply {
  RDMModel* m = [RDMModel shared];
  NSXPCConnection* from = [NSXPCConnection currentConnection];
  dispatch_async(m.queue, ^{
    NSDictionary* d = [NSJSONSerialization JSONObjectWithData:json options:0 error:nil];
    NSString* copyID = [d isKindOfClass:NSDictionary.class] ? d[@"copy"] : nil;
    NSArray* entries = [d isKindOfClass:NSDictionary.class] ? d[@"items"] : nil;
    if (![copyID isKindOfClass:NSString.class] || ![entries isKindOfClass:NSArray.class]) {
      reply(@"malformed");
      return;
    }
    if (from) [m.publishers setObject:from forKey:copyID];
    if ([m.publishedCopies containsObject:copyID]) {
      // Published again (the app reconnected to a new extension process,
      // or once more): what is known of it stays.
      reply(nil);
      return;
    }
    [m.publishedCopies addObject:copyID];
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
    [m.children[NSFileProviderRootContainerItemIdentifier] addObject:copyID];
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
    if (![m.publishedCopies containsObject:copyID]) return;
    [m dropCopy:copyID];
    [m.publishers removeObjectForKey:copyID];
    os_log(Log(), "withdrew %{public}@", copyID);
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
    [m.growing removeObjectForKey:itemID];
    RDMItem* item = m.items[itemID];
    // Ranges asked for get the whole file now.
    if (item) [m servePartials:itemID fd:handle.fileDescriptor available:item.size complete:YES];
    NSArray<RDMFetch*>* waiting = m.fetches[itemID];
    [m.fetches removeObjectForKey:itemID];
    if (!waiting.count || !item || !m.domain) {
      [handle closeFile];
      return;
    }
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

- (void)fileGrowing:(NSString*)itemID handle:(NSFileHandle*)handle {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    RDMGrowing* g = [[RDMGrowing alloc] init];
    g.handle = handle;
    m.growing[itemID] = g;
  });
}

- (void)progress:(NSString*)itemID bytes:(unsigned long long)bytes {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    for (RDMFetch* f in m.fetches[itemID]) f.progress.completedUnitCount = (int64_t)bytes;
    RDMGrowing* g = m.growing[itemID];
    if (g) {
      g.available = bytes;
      [m servePartials:itemID fd:g.handle.fileDescriptor available:bytes complete:NO];
    }
  });
}

- (void)itemFailed:(NSString*)itemID reason:(NSString*)reason {
  RDMModel* m = [RDMModel shared];
  dispatch_async(m.queue, ^{
    NSArray<RDMFetch*>* waiting = m.fetches[itemID];
    [m.fetches removeObjectForKey:itemID];
    NSArray<RDMPartial*>* parts = m.partials[itemID];
    [m.partials removeObjectForKey:itemID];
    [m.growing removeObjectForKey:itemID];
    os_log_error(Log(), "%{public}@ failed: %{public}@", itemID, reason);
    NSError* e = [NSError errorWithDomain:NSFileProviderErrorDomain
                                     code:NSFileProviderErrorServerUnreachable
                                 userInfo:@{NSLocalizedDescriptionKey : reason}];
    for (RDMFetch* f in waiting) f.completion(nil, nil, e);
    for (RDMPartial* p in parts) p.completion(nil, nil, NSMakeRange(0, 0), 0, e);
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

@interface FilesExtension : NSObject <NSFileProviderReplicatedExtension, NSFileProviderServicing,
                                      NSFileProviderPartialContentFetching>
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
  // The size up front, so the system shows a real fraction from the start.
  __block int64_t size = 1;
  dispatch_sync(m.queue, ^{
    RDMItem* it = m.items[itemIdentifier];
    if (it && it.size > 0) size = (int64_t)it.size;
  });
  NSProgress* progress = [NSProgress progressWithTotalUnitCount:size];
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

// A range of a file: what reads it (Finder copying) gets each piece as it
// arrives, so its progress is the transfer's and it writes as it goes.
- (NSProgress*)fetchPartialContentsForItemWithIdentifier:(NSFileProviderItemIdentifier)itemIdentifier
                                                 version:(NSFileProviderItemVersion*)requestedVersion
                                                 request:(NSFileProviderRequest*)request
                                            minimalRange:(NSRange)requestedRange
                                              aligningTo:(NSUInteger)alignment
                                                 options:(NSFileProviderFetchContentsOptions)options
                                       completionHandler:(void (^)(NSURL*, NSFileProviderItem, NSRange,
                                                                   NSFileProviderMaterializationFlags,
                                                                   NSError*))completionHandler {
  RDMModel* m = [RDMModel shared];
  NSProgress* progress = [NSProgress progressWithTotalUnitCount:(int64_t)MAX(requestedRange.length, (NSUInteger)1)];
  RDMPartial* part = [[RDMPartial alloc] init];
  part.range = requestedRange;
  part.alignment = alignment;
  part.progress = progress;
  __block BOOL completed = NO;
  part.completion = ^(NSURL* u, id<NSFileProviderItem> item, NSRange r, NSFileProviderMaterializationFlags f, NSError* e) {
    // On the model's queue; only the first answer counts.
    if (completed) return;
    completed = YES;
    completionHandler(u, item, r, f, e);
  };
  progress.cancellationHandler = ^{
    dispatch_async(m.queue, ^{
      [m.partials[itemIdentifier] removeObject:part];
      part.completion(nil, nil, NSMakeRange(0, 0), 0,
                      [NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
    });
  };
  dispatch_async(m.queue, ^{
    RDMItem* item = m.items[itemIdentifier];
    if (!item || item.kind != 'f') {
      part.completion(nil, nil, NSMakeRange(0, 0), 0,
                      [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil]);
      return;
    }
    os_log(Log(), "partial fetch %{public}@ at %lu, %lu bytes (alignment %lu)", itemIdentifier,
           (unsigned long)requestedRange.location, (unsigned long)requestedRange.length, (unsigned long)alignment);
    if (!m.partials[itemIdentifier]) m.partials[itemIdentifier] = [NSMutableArray array];
    [m.partials[itemIdentifier] addObject:part];
    RDMGrowing* g = m.growing[itemIdentifier];
    if (g) [m servePartials:itemIdentifier fd:g.handle.fileDescriptor available:g.available complete:NO];
    if (completed) return;
    NSXPCConnection* host = m.host;
    if (!host) {
      [m.partials[itemIdentifier] removeObject:part];
      part.completion(nil, nil, NSMakeRange(0, 0), 0,
                      [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorServerUnreachable userInfo:nil]);
      return;
    }
    [[host remoteObjectProxyWithErrorHandler:^(NSError* e) {
      (void)e;
      dispatch_async(m.queue, ^{
        [m.partials[itemIdentifier] removeObject:part];
        part.completion(nil, nil, NSMakeRange(0, 0), 0,
                        [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorServerUnreachable userInfo:nil]);
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
