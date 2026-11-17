// The application: the list of computers, the connect form, the menus, and
// making, keeping and remaking connections.
#pragma once
#import <AppKit/AppKit.h>

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate,
                                   NSMenuDelegate>
@end
