// What the File Provider extension (fileprovider/files_extension.m) and the
// app (session/src/file_target_mac.mm) say to each other over XPC
// (docs/file-paste.md). The app connects to the service the extension
// offers on its domain's root; on the same connection the extension calls
// the app back.
//
// Item identifiers: a copy's folder is "c<n>"; its top-level items are
// "c<n>/<index>"; anything inside one is "c<n>/<index>/<relative path>".
#import <Foundation/Foundation.h>

#define RDMFilesServiceName @"org.rdmachine.files.v1"

NS_ASSUME_NONNULL_BEGIN

// Exported by the extension.
@protocol RDMFilesExtension
// Replaces what is on offer: {"copy":"c3","items":[entries]} (file_transfer.h's
// entry JSON, a name each). The reply comes once the system was told; nil
// error = fine.
- (void)publish:(NSData*)json reply:(void (^)(NSString* _Nullable error))reply;
- (void)withdrawCopy:(NSString*)copyID;
// A top-level folder's whole tree: {"item":"c3/0","entries":[...]} (the item
// itself first, paths relative to it) or {"item":"c3/0","error":"..."}.
- (void)setTree:(NSData*)json;
// A file the extension asked for (fetchItem) has arrived: read it from `handle`.
- (void)fileReady:(NSString*)itemID handle:(NSFileHandle*)handle;
- (void)progress:(NSString*)itemID bytes:(unsigned long long)bytes;
- (void)itemFailed:(NSString*)itemID reason:(NSString*)reason;
@end

// Exported by the app.
@protocol RDMFilesHost
// The tree of a top-level folder ("c3/0") is needed.
- (void)listItem:(NSString*)topItemID;
// The content of a file is needed (any item identifier inside a copy).
- (void)fetchItem:(NSString*)itemID;
@end

NS_ASSUME_NONNULL_END
