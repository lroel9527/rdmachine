#include "rdmachine/session/login_screen.h"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>

namespace rdm {

namespace {

bool BoolValue(CFTypeRef v) {
  if (!v) return false;
  if (CFGetTypeID(v) == CFBooleanGetTypeID()) return CFBooleanGetValue(static_cast<CFBooleanRef>(v));
  if (CFGetTypeID(v) == CFNumberGetTypeID()) {
    int n = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &n);
    return n != 0;
  }
  return false;
}

}  // namespace

LoginScreenState QueryLoginScreen() {
  LoginScreenState s;
  if (CFDictionaryRef d = CGSessionCopyCurrentDictionary()) {
    // Not a documented constant, but the key the window server has used
    // since macOS 10.x for "the lock screen is up".
    s.screen_locked = BoolValue(CFDictionaryGetValue(d, CFSTR("CGSSessionScreenIsLocked")));
    CFRelease(d);
  }
  // System Settings > Users & Groups > "Login window shows: Name and
  // password" (the system-wide login window preferences).
  if (CFTypeRef v = CFPreferencesCopyAppValue(CFSTR("SHOWFULLNAME"), CFSTR("/Library/Preferences/com.apple.loginwindow"))) {
    s.asks_for_name = BoolValue(v);
    CFRelease(v);
  }
  return s;
}

}  // namespace rdm
