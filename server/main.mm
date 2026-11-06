// rdmachine-server: placeholder until the TLS control listener lands (M0).
// Prints the versions it was built with, so CI can check that the core,
// rdaccel and Apple frameworks link together.
#import <Foundation/Foundation.h>

#include <cstdio>

#include <rdaccel/rdaccel.h>

#include "rdmachine/protocol.h"

int main() {
  @autoreleasepool {
    std::printf("rdmachine-server %s (protocol %d, rdaccel %s, %s)\n", rdm::VersionString(), rdm::kProtocolVersion,
                rda_version_string(), rda_platform_name());
    std::printf("macOS %s\n", [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String]);
  }
  return 0;
}
