#import <Foundation/Foundation.h>

#include "rdmachine/session/client_input_icon.h"

namespace rdm {

namespace {

std::mutex& Mu() {
  static std::mutex mu;
  return mu;
}

ClientInputIcon& Current() {
  static ClientInputIcon icon;
  return icon;
}

}  // namespace

ClientInputIcon CurrentClientInputIcon() {
  std::lock_guard lock(Mu());
  return Current();
}

void SetClientInputIcon(ClientInputIcon icon) {
  {
    std::lock_guard lock(Mu());
    if (icon.fingerprint == Current().fingerprint) return;
    Current() = std::move(icon);
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSNotificationCenter.defaultCenter postNotificationName:@(kClientInputIconChangedNotification) object:nil];
  });
}

}  // namespace rdm
