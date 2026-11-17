// rdmachine-client: the Mac client. A connect window (server, account and
// password), then the remote screen in a window that sends
// this Mac's mouse, scroll wheel and keyboard back.
//
// Connecting goes through rdm::RemoteSession (session/), the same code the
// command-line rdmachine-connect and the tests use: TLS 1.3 control
// connection, the server's fingerprint checked against ~/Library/Application
// Support/rdmachine/known_hosts (a first-time fingerprint is shown for
// confirmation; a changed one is refused), login, then the rdaccel picture
// over UDP.
//
// The picture and input handling come from rdaccel's examples/remote_mac:
// pictures go into two IOSurfaces that Core Animation shows directly, each
// frame copying only what changed; keys are sent as physical keys (USB HID
// codes), so the remote Mac's layout and input method decide what is typed.
// Over the picture the pointer is this Mac's own, in the remote cursor's
// shape; outside the window the remote cursor is drawn where it is.
//
// The files: app_delegate (the list, the form, the menus, connecting),
// remote_window (the remote screen) and client_support (settings, saved
// computers and logins, helpers).
#include "app_delegate.h"
#include "client_support.h"

int main(int argc, char** argv) {
  @autoreleasepool {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--version") == 0) {
        std::printf("rdmachine-client %s (protocol %d, rdaccel %s, AppKit %.0f)\n", rdm::VersionString(),
                    rdm::kProtocolVersion, rda_version_string(), NSAppKitVersionNumber);
        return 0;
      }
      if (std::strcmp(argv[i], "--register-files") == 0) {
        // The installer, as the logged-in user: add the RDMachine location
        // so its switch is in System Settings (docs/file-paste.md).
        std::string error;
        const bool ok = rdm::RegisterFileDomain(&error);
        std::printf("RDMachine location: %s\n", ok ? "registered" : error.c_str());
        return ok ? 0 : 1;
      }
    }
    NSApplication* app = NSApplication.sharedApplication;
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    AppDelegate* delegate = [[AppDelegate alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
