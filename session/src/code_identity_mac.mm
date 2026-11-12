// PeerSignedLikeUs on macOS: the agent on the other end of the socket must
// satisfy this process's own designated requirement (same identifier and
// signing certificate: the same RDMachine Server.app), checked through the
// peer's audit token, which cannot be reused by another process the way a
// pid can.
#include "rdmachine/session/daemon.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <bsm/libbsm.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace rdm {

namespace {

// This process's designated requirement, or null when it is not signed in a
// way that has one (ad hoc, or unsigned development builds).
SecRequirementRef OwnRequirement() {
  static SecRequirementRef req = [] {
    SecCodeRef self = nullptr;
    SecStaticCodeRef stat = nullptr;
    SecRequirementRef r = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &self) == errSecSuccess &&
        SecCodeCopyStaticCode(self, kSecCSDefaultFlags, &stat) == errSecSuccess) {
      CFDictionaryRef info = nullptr;
      // Ad hoc signatures have a designated requirement of their own cdhash,
      // which no other binary meets; treat them as unsigned.
      if (SecCodeCopySigningInformation(stat, kSecCSSigningInformation, &info) == errSecSuccess && info) {
        const bool has_cert = CFDictionaryGetValue(info, kSecCodeInfoCertificates) != nullptr;
        CFRelease(info);
        if (has_cert && SecCodeCopyDesignatedRequirement(stat, kSecCSDefaultFlags, &r) != errSecSuccess) r = nullptr;
      }
    }
    if (stat) CFRelease(stat);
    if (self) CFRelease(self);
    return r;
  }();
  return req;
}

}  // namespace

bool PeerSignedLikeUs(int fd, std::string* why) {
  SecRequirementRef req = OwnRequirement();
  if (!req) {
    *why = "this daemon is not signed with a certificate: agents are not verified (development build)";
    return true;
  }
  audit_token_t token{};
  socklen_t len = sizeof token;
  if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &len) != 0) {
    *why = "cannot read the agent's audit token";
    return false;
  }
  NSData* data = [NSData dataWithBytes:&token length:sizeof token];
  NSDictionary* attrs = @{(__bridge NSString*)kSecGuestAttributeAudit : data};
  SecCodeRef code = nullptr;
  OSStatus st = SecCodeCopyGuestWithAttributes(nullptr, (__bridge CFDictionaryRef)attrs, kSecCSDefaultFlags, &code);
  if (st != errSecSuccess || !code) {
    *why = "cannot find the agent's code (" + std::to_string(st) + ")";
    return false;
  }
  st = SecCodeCheckValidity(code, kSecCSDefaultFlags, req);
  CFRelease(code);
  if (st != errSecSuccess) {
    *why = "the agent is not RDMachine Server signed like this daemon (" + std::to_string(st) + ")";
    return false;
  }
  why->clear();
  return true;
}

}  // namespace rdm
