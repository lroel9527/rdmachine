#pragma once
// The server's ServerDelegate: checks passwords with the system and runs one
// rdaccel host per session (capture, encode, input injection), admitting only
// the viewer that presents the session's one-time token.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <rdaccel/rdaccel.h>

#include "rdmachine/auth_guards.h"
#include "rdmachine/clipboard_sync.h"
#include "rdmachine/input_source_sync.h"
#include "rdmachine/server_session.h"
#include "rdmachine/session/virtual_desktop.h"

namespace rdm {

struct MediaHostOptions {
  std::string server_name;
  // rdaccel listens here: UDP on the control port (a different protocol, so
  // no clash), TCP fallback on the next port (docs/protocol.md).
  uint16_t udp_port = kDefaultPort;
  uint16_t tcp_port = kDefaultPort + 1;
  // Password check. Default: PAM (`pam_service`, empty = DefaultPamService())
  // and, in M0, only the account the server runs as (not_console_user).
  std::function<bool(const std::string& user, const std::string& password, std::string* error)> check_password;
  std::string pam_service;
  bool inject_input = true;  // false: every session is view-only
  // Clipboard shared with the client (docs/protocol.md, "剪贴板"); null =
  // not shared. kSend = only from this computer to the client. View-only
  // sessions never take the client's clipboard.
  std::shared_ptr<Clipboard> clipboard;
  ClipboardMode clipboard_mode = ClipboardMode::kBoth;
  // Builds the desktop a client asks for (docs/protocol.md, "请求桌面").
  // Empty = requests are not honoured: the display is shared as it is.
  VirtualDesktopFactory virtual_desktop;
  int32_t fps = 0;           // 0 = rdaccel's default
  int64_t bitrate_bps = 0;   // 0 = rdaccel's default
  std::function<void(const std::string&)> log;  // may be empty
};

// A snapshot of the running session, for status displays.
struct MediaHostStatus {
  bool running = false;          // an rdaccel host exists (session started)
  bool media_connected = false;  // the viewer's picture connection is up
  SessionOrigin origin;
  MediaTransport transport = MediaTransport::kUdp;
  bool view_only = false;          // in effect (asked for, or no input injection)
  bool text_mode = false;          // the picture is sent in text mode (docs/protocol.md, "文字模式")
  bool input_unavailable = false;  // control was wanted but input injection failed
                                   // (on macOS: no Accessibility permission)
  std::chrono::steady_clock::time_point started{};
  int32_t width = 0, height = 0;  // the shared display, pixels
  std::optional<DesktopSpec> desktop;  // the virtual desktop built for the client, if any
  std::string encoder;            // empty before the first frame
  // Keyboard input source sync (docs/protocol.md, "键盘语言同步"): what the
  // client last reported, and what this computer selected for it (empty:
  // nothing fits, or not synced yet).
  std::string client_input_source;
  std::string input_source;
  rda_host_stats stats{};         // cumulative; zero when not running
};

class MediaHost : public ServerDelegate {
 public:
  explicit MediaHost(MediaHostOptions options);
  ~MediaHost() override;  // ends a running session

  std::string ServerName() override;
  bool CheckPassword(const std::string& username, const std::string& password, std::string* error) override;
  std::optional<SessionGrant> StartSession(const SessionRequest& request, const SessionSecrets& secrets,
                                           const SessionOrigin& origin, std::string* error) override;
  void EndSession() override;

  // Whether an rdaccel host is running (for tests and status).
  bool session_running();
  // Thread-safe; cheap enough to poll a few times a second.
  MediaHostStatus Status();

 private:
  static int OnAuth(void* user, const uint8_t* token, size_t n);
  static void OnInput(void* user, const rda_input_event* ev);
  static void OnState(void* user, rda_state state, rda_status reason);
  static void OnError(void* user, rda_status status, const char* message);
  static void OnMessage(void* user, uint32_t type, const uint8_t* payload, size_t n);
  void SyncInputSource(const InputSourceState& wanted);
  // About once a second while a session runs: a SessionStatsReport to the
  // client for its overlay (docs/protocol.md, "会话统计").
  void StatsLoop();
  void StopStats();
  // The client's window changed (kDesktopRequestMessageType): the desktop is
  // rebuilt at the new size on a thread of its own, as capture has to stop
  // and start around it, which no rdaccel callback may do.
  void DesktopLoop();
  void StopDesktopLoop();
  void Log(const std::string& line);

  const MediaHostOptions options_;
  OneTimeTokens tokens_;
  std::mutex mu_;  // guards host_ and injector_ changes
  rda_host* host_ = nullptr;
  rda_injector* injector_ = nullptr;  // null for view-only sessions
  // Set before the host exists, cleared after it is destroyed (as injector_).
  std::unique_ptr<ClipboardSync> clipboard_;
  std::atomic<rda_host*> clipboard_host_{nullptr};  // where clipboard_ sends
  // The running host for OnMessage (transport thread, which must not take
  // mu_): set once the host exists, cleared before it is destroyed.
  std::atomic<rda_host*> mode_host_{nullptr};
  std::atomic<bool> text_mode_{false};
  MediaHostStatus status_;  // guarded by mu_; stats filled in by Status()
  std::atomic<bool> media_connected_{false};
  std::unique_ptr<VirtualDesktop> desktop_;  // guarded by mu_; outlives host_
  std::thread desktop_thread_;                // runs while desktop_ exists
  std::mutex desktop_mu_;
  std::condition_variable desktop_cv_;
  std::optional<DesktopSpec> desktop_wanted_;  // the latest request not yet acted on
  bool desktop_stop_ = false;
  std::thread stats_thread_;
  std::mutex stats_mu_;
  std::condition_variable stats_cv_;
  bool stats_stop_ = false;
};

}  // namespace rdm
