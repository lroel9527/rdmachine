#pragma once
// What this process is receiving of files pasted here (docs/file-paste.md,
// "接收进度"): Finder shows no progress while a pasted file is still on its
// way (it copies only once the download is over), so the apps show it in
// their menu bar icon instead.
#include <cstdint>
#include <string>

namespace rdm {

struct FileReceiveProgress {
  bool active = false;
  std::string name;   // what is being received (the first item, if several)
  int items = 0;      // top-level items being received
  uint64_t bytes = 0;
  uint64_t total = 0;  // 0: not known yet (a folder whose tree has not come)
  double bytes_per_second = 0;
};

// Thread-safe. Inactive in a process without the File Provider extension.
FileReceiveProgress CurrentFileReceive();

// "正在接收“报告.pdf” 23.4 / 60 MB · 2.3 MB/秒 · 约 16 秒".
std::string DescribeFileReceive(const FileReceiveProgress& p);

// macOS: posted (NSNotificationCenter, main thread) when it changes, a few
// times a second while something arrives and once when it ends.
inline constexpr const char* kFileReceiveChangedNotification = "RDMFileReceiveChanged";

}  // namespace rdm

#ifdef __OBJC__
#import <AppKit/AppKit.h>
// A menu bar icon for a receive under way: a ring filled to `fraction`
// (0...1; negative: not known, a short arc), as a template image.
NSImage* RDMFileProgressRing(double fraction);
#endif
