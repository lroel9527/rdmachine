// rdmachine-server: shares this computer's screen with rdmachine clients.
//
//   rdmachine-server [--port=4100] [--bind=ADDR] [--pairing-code] [--no-pairing]
//                    [--view-only] [--pam-service=NAME] [--config-dir=DIR]
//                    [--fps=N] [--bitrate-kbps=N] [--clipboard=MODE] [--headless]
//                    [--fingerprint] [--version]
//
// On macOS it puts an icon in the menu bar: connection status, pairing codes,
// the fingerprint, disconnect, settings and a floating statistics panel for
// debugging (server/mac_app.mm). --headless runs without it, as on Linux.
// Settings live in <config dir>/server-settings.json (the menu bar app edits
// them); the options above override them for one run.
//
// Listens for control connections (TLS 1.3, TCP --port) and serves one
// session at a time over rdaccel (UDP --port, TCP fallback --port + 1).
// Clients log in with the password of the account this server runs as, or
// with a pairing code: --pairing-code prints one at start, and pressing Enter
// prints a new one. The first run creates the server's identity in the
// config directory; --fingerprint prints it for comparing with what clients
// show.
//
// --clipboard shares the text clipboard with the client: both (default),
// send (only from this computer to the client), receive (only from the
// client) or off. View-only sessions never take the client's clipboard.
// Only macOS has a clipboard backend so far.
//
// macOS needs two permissions for the program that runs this: Screen
// Recording (capture) and Accessibility (input; without it sessions are
// view-only).
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#endif

#include <rdaccel/rdaccel.h>

#include "rdmachine/protocol.h"
#include "rdmachine/session/config_store.h"
#include "rdmachine/session/system_auth.h"
#include "server_runtime.h"
#if defined(__APPLE__)
#include "mac_app.h"
#endif

namespace {

std::mutex g_log_mu;

void Log(const std::string& line) {
  std::lock_guard lock(g_log_mu);
  const std::time_t t = std::time(nullptr);
  char ts[32];
  std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
  std::fprintf(stderr, "%s %s\n", ts, line.c_str());
}

void RdaLog(void*, rda_log_level level, const char* message) {
  if (level <= RDA_LOG_WARN) Log(std::string("rdaccel: ") + message);
}

bool Flag(const char* arg, const char* name, std::string* value) {
  const size_t n = std::strlen(name);
  if (std::strncmp(arg, name, n) != 0) return false;
  if (arg[n] == '\0' && !value) return true;
  if (arg[n] == '=' && value) {
    *value = arg + n + 1;
    return true;
  }
  return false;
}

std::string HostName() {
  char buf[256] = {};
  if (gethostname(buf, sizeof buf - 1) != 0) return "rdmachine";
  std::string h = buf;
  if (h.size() > 6 && h.compare(h.size() - 6, 6, ".local") == 0) h.resize(h.size() - 6);
  return h;
}

int Usage() {
  std::fprintf(stderr,
               "usage: rdmachine-server [--port=4100] [--bind=ADDR] [--pairing-code] [--no-pairing]\n"
               "                        [--view-only] [--pam-service=NAME] [--config-dir=DIR]\n"
               "                        [--fps=N] [--bitrate-kbps=N] [--clipboard=both|send|receive|off]\n"
               "                        [--headless] [--fingerprint] [--version]\n");
  return 2;
}

#if !defined(__APPLE__)
// Blocks until SIGINT or SIGTERM. The signals are blocked first, in main()
// before any thread starts, so that every thread inherits the mask and
// sigwait() gets them. (macOS: RunMenuBarApp / RunHeadlessApp; the main
// thread runs AppKit's loop there, which rdaccel's input injector needs to
// switch input sources and the process needs to follow display changes.)
sigset_t StopSignals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  return set;
}

void WaitForSignal() {
  const sigset_t set = StopSignals();
  int sig = 0;
  sigwait(&set, &sig);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  rdm::ServerRuntime::Options ropts;
  rdm::SettingsOverrides& ov = ropts.overrides;
  std::string config_dir = rdm::DefaultConfigDir();
  bool print_fingerprint = false, issue_code = false, headless = false;
  std::string v;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (Flag(a, "--version", nullptr)) {
      std::printf("rdmachine-server %s (protocol %d, rdaccel %s, %s)\n", rdm::VersionString(), rdm::kProtocolVersion,
                  rda_version_string(), rda_platform_name());
      return 0;
    } else if (Flag(a, "--port", &v)) {
      const int p = std::atoi(v.c_str());
      if (p <= 0 || p >= 65535) return Usage();
      ov.port = static_cast<uint16_t>(p);
    } else if (Flag(a, "--bind", &v)) {
      ropts.bind_address = v;
    } else if (Flag(a, "--pairing-code", nullptr)) {
      issue_code = true;
    } else if (Flag(a, "--no-pairing", nullptr)) {
      ov.offer_pairing_codes = false;
    } else if (Flag(a, "--view-only", nullptr)) {
      ov.view_only = true;
    } else if (Flag(a, "--pam-service", &v)) {
      ropts.pam_service = v;
    } else if (Flag(a, "--config-dir", &v)) {
      config_dir = v;
    } else if (Flag(a, "--fps", &v)) {
      ov.fps = std::atoi(v.c_str());
    } else if (Flag(a, "--bitrate-kbps", &v)) {
      ov.bitrate_kbps = std::atoi(v.c_str());
    } else if (Flag(a, "--clipboard", &v)) {
      auto m = rdm::ParseClipboardMode(v);
      if (!m) return Usage();
      ov.clipboard_mode = *m;
    } else if (Flag(a, "--headless", nullptr)) {
      headless = true;
    } else if (Flag(a, "--fingerprint", nullptr)) {
      print_fingerprint = true;
    } else {
      return Usage();
    }
  }
  if (issue_code && ov.offer_pairing_codes == false) return Usage();

  std::string error;
  auto identity = rdm::LoadOrCreateServerIdentity(config_dir, HostName(), &error);
  if (!identity) {
    std::fprintf(stderr, "rdmachine-server: %s\n", error.c_str());
    return 1;
  }
  const std::string fingerprint = rdm::CertificateFingerprint(identity->cert_pem).value_or("?");
  if (print_fingerprint) {
    std::printf("%s\n", rdm::FormatFingerprint(fingerprint).c_str());
    return 0;
  }

#if !defined(__APPLE__)
  const sigset_t stop = StopSignals();
  pthread_sigmask(SIG_BLOCK, &stop, nullptr);
#endif
  rda_set_log(&RdaLog, nullptr, RDA_LOG_WARN);
  ropts.config_dir = config_dir;
  ropts.server_name = HostName();
  ropts.log = &Log;
  rdm::ServerRuntime runtime(ropts, *identity);
  if (!runtime.Start(&error)) {
    std::fprintf(stderr, "rdmachine-server: %s\n", error.c_str());
#if defined(__APPLE__)
    if (!headless) rdm::ShowStartupError(error);
#endif
    return 1;
  }
  Log("rdmachine-server " + std::string(rdm::VersionString()) + " on port " + std::to_string(runtime.port()) + " as " +
      rdm::CurrentUserName());
  Log("fingerprint: " + rdm::FormatFingerprint(fingerprint));
  if (const std::string o = ov.Describe(); !o.empty()) Log("command line overrides the saved settings: " + o);
  if (issue_code) {
    Log("pairing code: " + runtime.IssuePairingCode() + " (valid 5 minutes; press Enter for a new one)");
    std::thread([&runtime] {
      std::string line;
      while (std::getline(std::cin, line)) Log("pairing code: " + runtime.IssuePairingCode());
    }).detach();
  }

#if defined(__APPLE__)
  if (!headless) {
    rdm::MenuBarAppInfo info;
    info.fingerprint = rdm::FormatFingerprint(fingerprint);
    info.user = rdm::CurrentUserName();
    info.version = rdm::VersionString();
    rdm::RunMenuBarApp(runtime, info);  // until Quit, SIGINT or SIGTERM
  } else {
    rdm::RunHeadlessApp();  // until SIGINT or SIGTERM
  }
#else
  (void)headless;
  WaitForSignal();
#endif
  Log("shutting down");
  runtime.Stop();
  return 0;
}
