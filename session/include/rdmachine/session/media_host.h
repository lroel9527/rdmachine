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
#include "rdmachine/ime_wire.h"
#include "rdmachine/input_source_sync.h"
#include "rdmachine/server_session.h"
#include "rdmachine/session/ime_bridge.h"
#include "rdmachine/session/ime_state_tracker.h"
#include "rdmachine/session/login_screen.h"
#include "rdmachine/session/virtual_desktop.h"

namespace rdm {

struct MediaHostOptions {
  std::string server_name;
  // rdaccel listens here: UDP on the control port (a different protocol, so
  // no clash; docs/protocol.md).
  uint16_t udp_port = kDefaultPort;
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
  // Where files the client copied appear when pasted here (null: as their
  // names), and where they are written on the way (docs/file-paste.md).
  std::shared_ptr<FileTarget> file_target;
  std::string file_staging_dir;
  // Builds the desktop a client asks for (docs/protocol.md, "请求桌面").
  // Empty = requests are not honoured: the display is shared as it is.
  VirtualDesktopFactory virtual_desktop;
  // Extra displays of an extended desktop (request.extra_desktops); each is
  // served on a port of its own: udp_port + 2 + i. Empty: none are built.
  ExtraDesktopFactory extra_desktop;
  // The server runs in the login window session (docs/decisions.md, "登录
  // 窗口和锁屏"): any account on this computer may log in with its password
  // (there is no logged-in user to match), and sessions say so to the
  // client (session_ready.login_window).
  bool login_window = false;
  // After a password login, type that password into the login window, or
  // into the lock screen when it is up, so the remote user gets straight to
  // the desktop. The password is kept in memory only between the login and
  // the start of the session, and wiped once typed.
  bool type_login = true;
  // Where the lock screen stands. Empty = QueryLoginScreen().
  std::function<LoginScreenState()> login_screen;
  int32_t fps = 0;           // 0 = rdaccel's default
  int64_t bitrate_bps = 0;   // 0 = rdaccel's default
  // Sound (docs/audio.md): sent to a client that asks for it (not from the
  // login window), with this computer silent meanwhile when mute_local_audio.
  // audio_source: RDA_AUDIO_SOURCE_SYSTEM, or _TEST (a computed signal, for
  // the automatic tests: no device is touched).
  bool send_audio = true;
  bool mute_local_audio = true;
  int32_t audio_source = RDA_AUDIO_SOURCE_SYSTEM;
  // Typing with the client's input method (docs/ime.md): the link to
  // RDMachine Input. Null: not offered (the login window, tests).
  std::shared_ptr<ImeBridge> ime;
  std::function<void(const std::string&)> log;  // may be empty
};

// A snapshot of the running session, for status displays.
struct MediaHostStatus {
  bool running = false;          // an rdaccel host exists (session started)
  bool media_connected = false;  // the viewer's picture connection is up
  SessionOrigin origin;
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
  // Sound: the session has it (session_ready.audio), and rdaccel's state of
  // it (zero when there is none).
  bool audio = false;
  // Without sound: why ("client": it did not ask; else as
  // session_ready.audio_reason: disabled, login_window, unsupported).
  std::string audio_reason;
  // The client's input method (docs/ime.md): empty when not offered;
  // "not enabled" (RDMachine Input is not added in System Settings),
  // "ready", or "in use" (the client types with it now).
  std::string ime;
  rda_audio_status audio_status{};
  rda_host_stats stats{};         // cumulative; zero when not running
};

// The session statistics' word for the sound's state (docs/protocol.md):
// off, starting, on, silent, paused, no_permission or failed.
const char* AudioStateName(bool session_has_audio, const rda_audio_status& s);

class MediaHost : public ServerDelegate {
 public:
  explicit MediaHost(MediaHostOptions options);
  ~MediaHost() override;  // ends a running session

  std::string ServerName() override;
  bool CheckPassword(const std::string& username, const std::string& password, std::string* error) override;
  std::optional<SessionGrant> StartSession(const SessionRequest& request, const SessionSecrets& secrets,
                                           const SessionOrigin& origin, std::string* error) override;
  void EndSession() override;

  // The password the next session's login was made with, when someone else
  // checked it (the daemon): typed into the login window or lock screen as
  // with type_login. Wiped after the next StartSession.
  void ExpectLogin(const std::string& username, const std::string& password);

  // Whether an rdaccel host is running (for tests and status).
  bool session_running();
  // Thread-safe; cheap enough to poll a few times a second. Never waits for
  // a session that is starting, ending or rebuilding its desktop (seconds):
  // it then returns the snapshot from before.
  MediaHostStatus Status();

 private:
  // An extra display of an extended desktop: its display, picture
  // connection and input (docs/protocol.md, "扩展桌面").
  struct Extra {
    MediaHost* self = nullptr;
    size_t index = 0;
    std::unique_ptr<VirtualDesktop> desktop;
    int32_t display_index = -1;  // rdaccel's
    ExtraDisplay info;
    rda_host* host = nullptr;
    rda_injector* injector = nullptr;
    std::mutex inject_mu;
  };
  static int OnExtraAuth(void* user, const uint8_t* token, size_t n);
  static void OnExtraInput(void* user, const rda_input_event* ev);
  static void OnExtraState(void* user, rda_state state, rda_status reason);
  static void OnExtraError(void* user, rda_status status, const char* message);
  // Builds the extra displays asked for, next to the main desktop (mu_ held).
  void BuildExtraDesktops(const SessionRequest& request);
  // Starts their hosts once the main one runs (mu_ held); those that cannot
  // start are dropped. Adds what works to `grant`.
  void StartExtraHosts(const SessionRequest& request, const SessionSecrets& secrets, const SessionOrigin& origin,
                       bool inject, SessionGrant* grant);
  static int OnAuth(void* user, const uint8_t* token, size_t n);
  static void OnInput(void* user, const rda_input_event* ev);
  static void OnState(void* user, rda_state state, rda_status reason);
  static void OnError(void* user, rda_status status, const char* message);
  static void OnMessage(void* user, uint32_t type, const uint8_t* payload, size_t n);
  void SyncInputSource(const InputSourceState& wanted);
  // The client's input method (docs/ime.md). OnImeInput runs on the
  // transport thread, in order with the keys; the bridge's events on its own.
  void OnImeInput(const ImeInput& in);
  void OnImeReport(const ImeBridge::Report& r);
  // Tells the client what changed (all of it when `force`), on the message
  // channel of the main picture.
  void SendImeState(bool force);
  // Types text as characters (no text field takes input methods, or the
  // input method refused it).
  void TypeText(const std::string& text);
  // Leaves the input method mode. restore: the user chose another mode, and
  // the input source from before comes back; otherwise (the session ends)
  // this Mac stays on RDMachine Input: the input source changes only when
  // the user changes the mode (6.5.0, docs/ime.md).
  void EndImeMode(const char* why, bool restore);
  bool ImeSourceEnabled();
  void ImeAfterInput(const rda_input_event* ev);
  // About once a second while a session runs: a SessionStatsReport to the
  // client for its overlay (docs/protocol.md, "会话统计").
  void StatsLoop();
  void StopStats();
  // The client's window changed (kDesktopRequestMessageType): the desktop is
  // rebuilt at the new size on a thread of its own, as capture has to stop
  // and start around it, which no rdaccel callback may do.
  void DesktopLoop();
  void StopDesktopLoop();
  // macOS may stop the screen capture stream by itself (the screen locked or
  // the display went to sleep after a long idle). The picture then stays on
  // the last frame while input still arrives, until capture starts again:
  // this thread restarts it, every 2 seconds until it works (OnError asks).
  void CaptureLoop();
  void StopCaptureLoop();
  // Types the login of a password session into the login window or the lock
  // screen, once the viewer is connected (its own thread; see type_login).
  void TypeLogin(std::string username, std::string password, bool type_name, bool at_login_window);
  void StopTypeLogin();
  void LogInput();  // the input summary line, if anything came since the last
  void Inject(const rda_input_event& ev);
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
  std::atomic<bool> audio_{false};  // the running session has sound
  std::string last_audio_state_;    // StatsLoop's thread only
  MediaHostStatus status_;  // guarded by mu_; stats filled in by Status()
  // What Status() last returned, for a caller that would otherwise wait.
  std::mutex snapshot_mu_;
  MediaHostStatus snapshot_;
  // Input source sync, written on the transport thread: under a lock of its
  // own, as that thread must not wait for mu_ (EndSession holds mu_ while it
  // waits for that very thread to finish).
  std::mutex input_source_mu_;
  std::string client_input_source_, input_source_;
  // The client's input method. Its own lock: the transport thread and the
  // bridge's thread both use it, and neither may wait for mu_.
  std::mutex ime_mu_;
  ImeStateTracker ime_;          // what is known now
  uint64_t ime_last_input_ = 0;  // the client's last ImeInput (for an input method that does not say)
  ImeState ime_last_sent_;       // what the client was last told
  bool ime_sent_ = false;        // something went out this session
  bool ime_available_ = false;   // this session can take the client's input method
  bool ime_mode_ = false;        // the client types with its input method now
  std::atomic<bool> ime_mode_on_{false};
  std::atomic<bool> ime_debug_{false};  // ImeDebugLog: positions and composition messages to the log
  // The input source RDMachine Input was selected in place of: it comes back
  // when the user picks another mode, also in a later session (a session
  // ending leaves RDMachine Input selected). Empty = none known.
  std::string ime_prev_source_;
  // Held while the input source is changed for the mode (selecting RDMachine
  // Input, putting the one from before back): those calls wait for the main
  // thread, which takes ime_mu_ for the menu, so they must not run under
  // ime_mu_. Never taken on the main thread while a session runs. Taken
  // before ime_mu_.
  std::mutex ime_source_mu_;
  std::vector<int32_t> ime_displays_;  // rdaccel's display index of each session display (0 = main)
  std::atomic<bool> media_connected_{false};
  std::unique_ptr<VirtualDesktop> desktop_;  // guarded by mu_; outlives host_
  std::unique_ptr<DisplayAwake> awake_;      // guarded by mu_; while a session runs
  std::vector<std::unique_ptr<Extra>> extras_;  // guarded by mu_
  std::thread desktop_thread_;                // runs while desktop_ exists
  std::thread capture_thread_;                // runs while a session does
  std::mutex capture_mu_;
  std::condition_variable capture_cv_;
  bool capture_restart_ = false;
  bool capture_stop_ = false;
  std::mutex desktop_mu_;
  std::condition_variable desktop_cv_;
  std::optional<DesktopSpec> desktop_wanted_;  // the latest request not yet acted on
  bool desktop_stop_ = false;
  // The last password login, until StartSession takes it (see type_login).
  struct PendingLogin {
    std::string username, password;
    std::chrono::steady_clock::time_point at{};
  };
  std::mutex login_mu_;
  PendingLogin pending_login_;
  std::thread type_thread_;
  std::mutex type_mu_;
  std::condition_variable type_cv_;
  bool type_stop_ = false;
  std::mutex inject_mu_;  // the client's input and TypeLogin take turns
  // The client's input as it arrives (guarded by inject_mu_), logged every
  // few seconds while it changes: whether it gets here, and whether
  // injecting it works (the login window: docs/decisions.md).
  uint64_t input_events_ = 0, input_failed_ = 0, input_logged_ = 0;
  uint32_t input_kinds_ = 0;  // bit per rda_input_kind seen
  std::string input_error_;
  std::thread stats_thread_;
  std::mutex stats_mu_;
  std::condition_variable stats_cv_;
  bool stats_stop_ = false;
};

}  // namespace rdm
