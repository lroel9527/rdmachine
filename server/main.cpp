// rdmachine-server: shares this computer's screen with rdmachine clients.
//
//   rdmachine-server [--port=4100] [--bind=ADDR] [--pairing-code] [--no-pairing]
//                    [--view-only] [--pam-service=NAME] [--config-dir=DIR]
//                    [--fps=N] [--bitrate-kbps=N] [--clipboard=MODE] [--headless]
//                    [--fingerprint] [--version] [--standalone]
//   rdmachine-server --daemon [--port=N] [--config-dir=DIR]   (root, launchd)
//   rdmachine-server --login-window                            (root, launchd)
//
// Three ways to run (docs/decisions.md, "守护进程和 agent"):
//   --daemon        the rdmachine daemon: root, from startup; owns the port,
//                   the identity (/Library/Application Support/rdmachine) and
//                   the logins, and hands each session to the agent of the
//                   graphical session that owns the console.
//   agent           a graphical session's picture: the menu bar app in a
//                   user's session when the daemon is installed (its socket
//                   exists), or --login-window in the login window session.
//   standalone      no daemon (or --standalone): its own control server, as
//                   before 3.0.
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
// --clipboard shares the clipboard (text, rich text and images; copied
// files only as their names) with the client: both (default), send (only
// from this computer to the client), receive (only from the client) or off.
// What is copied only crosses when it is pasted on the other side.
// View-only sessions never take the client's clipboard. Only macOS has a
// clipboard backend so far.
//
// macOS needs two permissions for the program that runs this: Screen
// Recording (capture) and Accessibility (input; without it sessions are
// view-only).
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#endif

#include <rdaccel/rdaccel.h>

#include "rdmachine/protocol.h"
#include "rdmachine/session/server_settings.h"
#include "rdmachine/session/daemon.h"
#include "rdmachine/control_server.h"
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
               "                        [--headless] [--fingerprint] [--version] [--standalone]\n"
               "       rdmachine-server --daemon [--port=N] [--config-dir=DIR]\n"
               "       rdmachine-server --login-window\n");
  return 2;
}

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

// --daemon: the network side of a server installed by rdmachine-service.
int RunDaemon(const std::string& config_dir, std::optional<uint16_t> port_override, const std::string& pam_service,
              const std::string& socket_path, bool issue_code) {
  // Before any thread starts, so that sigwait() gets the signals.
  const sigset_t stop = StopSignals();
  pthread_sigmask(SIG_BLOCK, &stop, nullptr);
  rda_set_log(&RdaLog, nullptr, RDA_LOG_WARN);
  std::string error;
  auto identity = rdm::LoadOrCreateServerIdentity(config_dir, HostName(), &error);
  if (!identity) {
    std::fprintf(stderr, "rdmachine-server --daemon: %s\n", error.c_str());
    return 1;
  }
  const std::string fingerprint = rdm::FormatFingerprint(rdm::CertificateFingerprint(identity->cert_pem).value_or("?"));
  rdm::ServerSettings settings;
  if (!rdm::LoadServerSettings(config_dir, &settings, &error)) {
    Log("settings: " + error + "; using the defaults");
    settings = rdm::ServerSettings{};
  }
  if (port_override) settings.port = *port_override;

  std::mutex mu;  // guards server and settings
  std::unique_ptr<rdm::ControlServer> server;
  std::unique_ptr<rdm::Daemon> daemon;
  auto start_server = [&](std::string* err) {
    rdm::ControlServerOptions co;
    co.port = settings.port;
    co.offer_pairing_codes = settings.offer_pairing_codes;
    auto s = std::make_unique<rdm::ControlServer>(*daemon, *identity, co);
    if (!s->Start(err)) return false;
    server = std::move(s);
    Log("listening on port " + std::to_string(server->port()) +
        (settings.offer_pairing_codes ? "" : ", no pairing codes"));
    return true;
  };

  rdm::DaemonOptions d;
  d.server_name = HostName();
  d.socket_path = socket_path;
  d.pam_service = pam_service;
  d.log = &Log;
  d.fingerprint = [fingerprint] { return fingerprint; };
  d.port = [&] {
    std::lock_guard l(mu);
    return server ? server->port() : settings.port;
  };
  d.issue_pairing_code = [&] {
    std::lock_guard l(mu);
    return server ? server->IssuePairingCode() : std::string();
  };
  d.pairing_code_remaining_ms = [&]() -> int64_t {
    std::lock_guard l(mu);
    return server ? std::chrono::duration_cast<std::chrono::milliseconds>(server->PairingCodeRemaining()).count() : 0;
  };
  d.close_connections = [&] {
    std::lock_guard l(mu);
    if (server) server->CloseConnections();
  };
  d.settings_changed = [&](uint16_t port, bool offer) {
    if (port_override) return;  // the command line wins
    // Not on the agent's reader thread: restarting the server ends the
    // session, which waits for that agent's answer.
    std::thread([&, port, offer] {
      std::lock_guard l(mu);
      if (port == settings.port && offer == settings.offer_pairing_codes) return;
      rdm::ServerSettings next = settings;
      next.port = port;
      next.offer_pairing_codes = offer;
      std::string err;
      if (server) server->Stop();
      server.reset();
      const rdm::ServerSettings before = settings;
      settings = next;
      if (!start_server(&err)) {
        Log("cannot use port " + std::to_string(port) + ": " + err + "; staying on " + std::to_string(before.port));
        settings = before;
        start_server(&err);
        return;
      }
      rdm::SaveServerSettings(config_dir, settings, &err);
    }).detach();
  };
  daemon = std::make_unique<rdm::Daemon>(d);
  if (!daemon->Start(&error)) {
    std::fprintf(stderr, "rdmachine-server --daemon: %s\n", error.c_str());
    return 1;
  }
  {
    std::lock_guard l(mu);
    // A port still held by a server from before the install: keep trying.
    bool ok = start_server(&error);
    for (int i = 0; !ok && i < 60; ++i) {
      if (i == 0) Log("cannot listen yet (" + error + "); trying again for a minute");
      mu.unlock();
      std::this_thread::sleep_for(std::chrono::seconds(1));
      mu.lock();
      ok = start_server(&error);
    }
    if (!ok) {
      std::fprintf(stderr, "rdmachine-server --daemon: %s\n", error.c_str());
      daemon->Stop();
      return 1;  // launchd tries again
    }
  }
  Log("rdmachine daemon " + std::string(rdm::VersionString()) + ", fingerprint " + fingerprint);
  if (issue_code) {
    std::lock_guard l(mu);
    if (server) Log("pairing code: " + server->IssuePairingCode() + " (valid 5 minutes)");
  }
  WaitForSignal();
  Log("shutting down");
  {
    std::lock_guard l(mu);
    if (server) server->Stop();  // ends the session through the daemon
    server.reset();
  }
  daemon->Stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  rdm::ServerRuntime::Options ropts;
  rdm::SettingsOverrides& ov = ropts.overrides;
  std::string config_dir = rdm::DefaultConfigDir();
  bool print_fingerprint = false, issue_code = false, headless = false, config_dir_given = false;
  bool daemon_mode = false, standalone = false;
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
      config_dir_given = true;
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
    } else if (Flag(a, "--daemon", nullptr)) {
      daemon_mode = true;
    } else if (Flag(a, "--agent-socket", &v)) {
      ropts.agent_socket = v;  // where the daemon and its agents meet (tests)
    } else if (Flag(a, "--standalone", nullptr)) {
      standalone = true;
    } else if (Flag(a, "--login-window", nullptr)) {
      // The agent of the login window session (LaunchAgent,
      // LimitLoadToSessionType LoginWindow): no menu bar, no clipboard.
      ropts.login_window = true;
      ropts.agent_kind = "loginwindow";
      headless = true;
    } else {
      return Usage();
    }
  }
  if (daemon_mode) {
    if (!config_dir_given) config_dir = rdm::SystemConfigDir();
    if (print_fingerprint) {
      std::string error;
      auto identity = rdm::LoadOrCreateServerIdentity(config_dir, HostName(), &error);
      if (!identity) return 1;
      std::printf("%s\n", rdm::FormatFingerprint(rdm::CertificateFingerprint(identity->cert_pem).value_or("?")).c_str());
      return 0;
    }
    return RunDaemon(config_dir, ov.port, ropts.pam_service, ropts.agent_socket, issue_code);
  }
  // A user's session: an agent of the daemon when one is installed.
  struct stat sock {};
  if (ropts.agent_kind.empty() && !standalone && stat(ropts.agent_socket.c_str(), &sock) == 0) {
    ropts.agent_kind = "aqua";
  }
  const bool agent = !ropts.agent_kind.empty();
  if (issue_code && (ov.offer_pairing_codes == false || ropts.login_window)) return Usage();

#if defined(__APPLE__)
  if (!headless && rdm::AnotherInstanceRunning()) {
    Log("rdmachine-server already runs in this session; leaving it to that one");
    return 0;  // a clean exit: a LaunchAgent with KeepAlive SuccessfulExit=false does not restart it
  }
#endif
  std::string error;
  std::optional<rdm::TlsIdentity> identity;
  std::string fingerprint = "?";
  if (agent) {
    identity = rdm::TlsIdentity{};  // the daemon's identity is the one clients see
  } else {
    identity = rdm::LoadOrCreateServerIdentity(config_dir, HostName(), &error);
    if (!identity) {
      std::fprintf(stderr, "rdmachine-server: %s\n", error.c_str());
      return 1;
    }
    fingerprint = rdm::CertificateFingerprint(identity->cert_pem).value_or("?");
  }
  if (print_fingerprint) {
    if (agent) {
      std::fprintf(stderr, "the daemon holds the identity: rdmachine-server --daemon --fingerprint (sudo)\n");
      return 1;
    }
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
  // Started by launchd (rdmachine-service): never stop at a dialog no one
  // may be there to answer.
  const char* service = std::getenv("XPC_SERVICE_NAME");
  const bool by_launchd = service && std::strncmp(service, "org.rdmachine.", 14) == 0;
  bool started = runtime.Start(&error);
  for (int i = 0; !started && by_launchd && i < 30; ++i) {
    if (i == 0) Log("cannot start yet (" + error + "); trying again for 30 seconds");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    started = runtime.Start(&error);
  }
  if (!started) {
    std::fprintf(stderr, "rdmachine-server: %s\n", error.c_str());
#if defined(__APPLE__)
    if (!headless && !by_launchd) rdm::ShowStartupError(error);
#endif
    return 1;  // launchd starts it again (KeepAlive: SuccessfulExit = false)
  }
  if (agent) {
    Log("rdmachine-server " + std::string(rdm::VersionString()) + " (" + ropts.agent_kind + " agent) as " +
        rdm::CurrentUserName());
    // The menu shows the daemon's fingerprint once the link is up.
    for (int i = 0; i < 30 && !runtime.daemon_connected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (runtime.daemon_connected()) fingerprint = runtime.daemon_fingerprint();
  } else {
    Log("rdmachine-server " + std::string(rdm::VersionString()) + " on port " + std::to_string(runtime.port()) + " as " +
        rdm::CurrentUserName());
    fingerprint = rdm::FormatFingerprint(fingerprint);
  }
  Log("fingerprint: " + fingerprint);
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
    info.fingerprint = fingerprint;
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
