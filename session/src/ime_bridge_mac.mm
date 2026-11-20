// The macOS pieces of the input method link (docs/ime.md, 7): who may
// connect, secure input, where a rectangle is in a display's pixels, and
// registering RDMachine Input with Text Input Sources.
#include "rdmachine/session/ime_bridge.h"

#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <CommonCrypto/CommonDigest.h>
#include <bsm/libbsm.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cmath>

namespace rdm {

namespace {

// "certificate leaf = H\"<sha1 of our signing certificate>\"": RDMachine
// Server and RDMachine Input are different bundles signed with the same
// (self-made, fixed) certificate. Null when this program has none.
SecRequirementRef SameCertificateRequirement() {
  static SecRequirementRef req = [] {
    SecCodeRef self = nullptr;
    SecStaticCodeRef stat = nullptr;
    CFDictionaryRef info = nullptr;
    SecRequirementRef r = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &self) == errSecSuccess &&
        SecCodeCopyStaticCode(self, kSecCSDefaultFlags, &stat) == errSecSuccess &&
        SecCodeCopySigningInformation(stat, kSecCSSigningInformation, &info) == errSecSuccess && info) {
      NSArray* certs = (__bridge NSArray*)CFDictionaryGetValue(info, kSecCodeInfoCertificates);
      if (certs.count > 0) {
        SecCertificateRef leaf = (__bridge SecCertificateRef)certs[0];
        NSData* der = CFBridgingRelease(SecCertificateCopyData(leaf));
        unsigned char sha1[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1(der.bytes, static_cast<CC_LONG>(der.length), sha1);
        {
          NSMutableString* hex = [NSMutableString string];
          for (unsigned char c : sha1) [hex appendFormat:@"%02x", c];
          NSString* text = [NSString stringWithFormat:@"certificate leaf = H\"%@\"", hex];
          if (SecRequirementCreateWithString((__bridge CFStringRef)text, kSecCSDefaultFlags, &r) != errSecSuccess) {
            r = nullptr;
          }
        }
      }
    }
    if (info) CFRelease(info);
    if (stat) CFRelease(stat);
    if (self) CFRelease(self);
    return r;
  }();
  return req;
}

}  // namespace

bool PeerSignedWithOurCertificate(int fd, std::string* why) {
  SecRequirementRef req = SameCertificateRequirement();
  if (!req) {
    *why = "this program is not signed with a certificate: the input method is not verified (development build)";
    return true;
  }
  audit_token_t token{};
  socklen_t len = sizeof token;
  if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &len) != 0) {
    *why = "cannot read the peer's audit token";
    return false;
  }
  NSData* data = [NSData dataWithBytes:&token length:sizeof token];
  NSDictionary* attrs = @{(__bridge NSString*)kSecGuestAttributeAudit : data};
  SecCodeRef code = nullptr;
  OSStatus st = SecCodeCopyGuestWithAttributes(nullptr, (__bridge CFDictionaryRef)attrs, kSecCSDefaultFlags, &code);
  if (st != errSecSuccess || !code) {
    *why = "cannot find the peer's code (" + std::to_string(st) + ")";
    return false;
  }
  st = SecCodeCheckValidity(code, kSecCSDefaultFlags, req);
  CFRelease(code);
  if (st != errSecSuccess) {
    *why = "the peer is not signed with this program's certificate (" + std::to_string(st) + ")";
    return false;
  }
  why->clear();
  return true;
}

bool SecureInputActive() { return IsSecureEventInputEnabled(); }

bool GlobalRectToDisplayPixels(const ImeBridge::GlobalRect& r, int32_t* origin_x, int32_t* origin_y, ImeRect* px) {
  const CGPoint p = CGPointMake(r.x, r.y + r.height / 2);
  CGDirectDisplayID id = 0;
  uint32_t n = 0;
  if (CGGetDisplaysWithPoint(p, 1, &id, &n) != kCGErrorSuccess || n == 0) return false;
  const CGRect b = CGDisplayBounds(id);
  if (b.size.width <= 0) return false;
  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
  const double pixel_width = mode ? static_cast<double>(CGDisplayModeGetPixelWidth(mode)) : b.size.width;
  if (mode) CGDisplayModeRelease(mode);
  const double scale = pixel_width / b.size.width;
  *origin_x = static_cast<int32_t>(std::lround(b.origin.x));
  *origin_y = static_cast<int32_t>(std::lround(b.origin.y));
  px->x = static_cast<int32_t>(std::lround((r.x - b.origin.x) * scale));
  px->y = static_cast<int32_t>(std::lround((r.y - b.origin.y) * scale));
  px->width = std::max<int32_t>(1, static_cast<int32_t>(std::lround(r.width * scale)));
  px->height = std::max<int32_t>(1, static_cast<int32_t>(std::lround(r.height * scale)));
  return true;
}

std::shared_ptr<ImeBridge> SystemImeBridge(std::function<void(const std::string&)> log) {
  // Installed by the package; registering tells Text Input Sources at once
  // (the user still has to add it once in System Settings, docs/ime.md 7.5).
  NSURL* url = [NSURL fileURLWithPath:@"/Library/Input Methods/RDMachine Input.app"];
  if ([url checkResourceIsReachableAndReturnError:nil]) {
    const OSStatus st = TISRegisterInputSource((__bridge CFURLRef)url);
    if (st != noErr && log) log("input method: registering it failed (" + std::to_string(st) + ")");
  }
  auto bridge = std::make_shared<ImeBridge>(ImeSocketPath(), &PeerSignedWithOurCertificate, log);
  std::string err;
  if (!bridge->Start(&err)) {
    if (log) log("input method: cannot listen: " + err);
    return nullptr;
  }
  return bridge;
}

}  // namespace rdm
