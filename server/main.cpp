// rdmachine-server: shares this computer's screen with rdmachine clients.
//
//   rdmachine-server [--port=4100] [--bind=ADDR] [--pairing-code] [--no-pairing]
//                    [--view-only] [--pam-service=NAME] [--config-dir=DIR]
//                    [--fps=N] [--bitrate-kbps=N] [--fingerprint] [--version]
//
// Listens for control connections (TLS 1.3, TCP --port) and serves one
// session at a time over rdaccel (UDP --port, TCP fallback --port + 1).
// Clients log in with the password of the account this server runs as, or
// with a pairing code: --pairing-code prints one at start, and pressing Enter
// prints a new one. The first run creates the server's identity in the
// config directory; --fingerprint prints it for comparing with what clients
// show.
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

#include "rdmachine/control_server.h"
#include "rdmachine/protocol.h"
#include "rdmachine/session/config_store.h"
#include "rdmachine/session/media_host.h"
#include "rdmachine/session/system_auth.h"

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
               "                        [--fps=N] [--bitrate-kbps=N] [--fingerprint] [--version]\n");
  return 2;
}

#if defined(__APPLE__)
void StopMainLoop(void*) { CFRunLoopStop(CFRunLoopGetMain()); }
#endif

// Blocks until SIGINT or SIGTERM. On macOS the main thread runs a run loop
// meanwhile: rdaccel's input injector needs one to switch input sources
// while typing text.
#if !defined(__APPLE__)
// On Linux the signals are blocked first, in main() before any thread
// starts, so that every thread inherits the mask and sigwait() gets them.
sigset_t StopSignals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  return set;
}
#endif

void WaitForSignal() {
#if defined(__APPLE__)
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  for (int sig : {SIGINT, SIGTERM}) {
    dispatch_source_t src =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, static_cast<uintptr_t>(sig), 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler_f(src, &StopMainLoop);
    dispatch_resume(src);
  }
  CFRunLoopRun();
#else
  const sigset_t set = StopSignals();
  int sig = 0;
  sigwait(&set, &sig);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  rdm::ControlServerOptions copts;
  rdm::MediaHostOptions mopts;
  std::string config_dir = rdm::DefaultConfigDir();
  bool print_fingerprint = false, issue_code = false;
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
      copts.port = static_cast<uint16_t>(p);
    } else if (Flag(a, "--bind", &v)) {
      copts.bind_address = v;
    } else if (Flag(a, "--pairing-code", nullptr)) {
      issue_code = true;
    } else if (Flag(a, "--no-pairing", nullptr)) {
      copts.offer_pairing_codes = false;
    } else if (Flag(a, "--view-only", nullptr)) {
      mopts.inject_input = false;
    } else if (Flag(a, "--pam-service", &v)) {
      mopts.pam_service = v;
    } else if (Flag(a, "--config-dir", &v)) {
      config_dir = v;
    } else if (Flag(a, "--fps", &v)) {
      mopts.fps = std::atoi(v.c_str());
    } else if (Flag(a, "--bitrate-kbps", &v)) {
      mopts.bitrate_bps = std::atoll(v.c_str()) * 1000;
    } else if (Flag(a, "--fingerprint", nullptr)) {
      print_fingerprint = true;
    } else {
      return Usage();
    }
  }
  if (issue_code && !copts.offer_pairing_codes) return Usage();

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
  mopts.server_name = HostName();
  mopts.udp_port = copts.port;
  mopts.tcp_port = static_cast<uint16_t>(copts.port + 1);
  mopts.log = &Log;
  rdm::MediaHost media(mopts);
  rdm::ControlServer server(media, *identity, copts);
  if (!server.Start(&error)) {
    std::fprintf(stderr, "rdmachine-server: %s\n", error.c_str());
    return 1;
  }
  Log("rdmachine-server " + std::string(rdm::VersionString()) + " listening on port " + std::to_string(server.port()) +
      " as " + rdm::CurrentUserName());
  Log("fingerprint: " + rdm::FormatFingerprint(fingerprint));
  if (issue_code) {
    Log("pairing code: " + server.IssuePairingCode() + " (valid 5 minutes; press Enter for a new one)");
    std::thread([&server] {
      std::string line;
      while (std::getline(std::cin, line)) Log("pairing code: " + server.IssuePairingCode());
    }).detach();
  }

  WaitForSignal();
  Log("shutting down");
  server.Stop();
  return 0;
}
