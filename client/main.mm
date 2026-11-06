// rdmachine-client: placeholder until the AppKit client lands (M0).
#import <AppKit/AppKit.h>

#include <cstdio>

#include <rdaccel/rdaccel.h>

#include "rdmachine/protocol.h"

int main() {
  @autoreleasepool {
    std::printf("rdmachine-client %s (protocol %d, rdaccel %s, AppKit %.0f)\n", rdm::VersionString(),
                rdm::kProtocolVersion, rda_version_string(), NSAppKitVersionNumber);
  }
  return 0;
}
