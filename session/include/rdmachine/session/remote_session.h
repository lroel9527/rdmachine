#pragma once
// Client side of a whole session: control connection (with the known-hosts
// check), the rdaccel viewer, UDP to TCP fallback, and keeping the control
// connection alive until either side ends the session.
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rdaccel/rdaccel.h>

#include "rdmachine/clipboard_sync.h"
#include "rdmachine/control_client.h"
#include "rdmachine/session/known_hosts.h"

namespace rdm {

// A trust_server callback backed by known_hosts: a pinned fingerprint must
// match; an unknown one is passed to `ask_first_time`, and remembered if it
// returns true. A changed one is reported through *changed and refused,
// unless `ask_changed` (old, new) accepts it: then the new one replaces the
// old (the user trusts the server's new identity, e.g. after a reinstall).
std::function<bool(const std::string&)> TrustWithKnownHosts(
    KnownHosts* known_hosts, std::string key, std::function<bool(const std::string& fingerprint)> ask_first_time,
    std::shared_ptr<bool> changed,
    std::function<bool(const std::string& old_fingerprint, const std::string& new_fingerprint)> ask_changed = {});

class RemoteSession {
 public:
  struct Options {
    ClientOptions control;    // request.transport is where it starts (UDP)
    rda_viewer_config viewer;  // the application's callbacks (rda_viewer_config_default first)
    // Clipboard shared with the server (docs/protocol.md, "剪贴板"); null =
    // not shared. Only copies made after connecting are offered, and only
    // fetched when pasted over there. Clipboard messages are then not passed
    // to viewer.on_message.
    std::shared_ptr<Clipboard> clipboard;
    ClipboardMode clipboard_mode = ClipboardMode::kBoth;
    // Fetch what the server copies right away instead of when it is pasted
    // (a process with no main run loop, such as the command-line client).
    bool clipboard_fetch_on_announce = false;
    std::function<void(const std::string&)> log;  // may be empty
    int udp_wait_ms = 4000;    // then fall back to TCP; 0 = no fallback
    // The session ended from the other side or the network: "server_closed",
    // "connection_lost", "media_lost", or the server's bye reason. Runs on
    // the session's own thread: do not destroy the session from it.
    std::function<void(const std::string& reason)> on_ended;
    // The server moved the session to another graphical session of that
    // computer (docs/protocol.md, "会话迁移"): there is a new picture
    // connection (the old one's frames stop), and what the application told
    // the old one (desktop size, input source, picture mode) has to be said
    // again. Runs on the session's own thread.
    std::function<void(const SessionReady& session)> on_moved;
    // The server said the session is moving (session_moving, with its
    // reason); on_moved or on_ended follows. Session thread.
    std::function<void(const std::string& reason)> on_moving;
    // true: nothing heard from the server for a few seconds (the network or
    // the server may be gone; on_ended follows if it stays silent); false:
    // heard again. Session thread.
    std::function<void(bool stalled)> on_stalled;
  };

  // Connects and waits until the picture's connection is up. On failure
  // returns null with *result describing why (result->error).
  static std::unique_ptr<RemoteSession> Start(Options options, ConnectResult* result);

  ~RemoteSession();  // Close()

  // Sends through the current picture connection (it changes when the
  // session moves, so do not keep the viewer pointer). Thread-safe.
  rda_status SendInput(const rda_input_event& ev);
  rda_status SendMessage(uint32_t type, const uint8_t* payload, size_t n);
  // The current viewer, for statistics and tests; may change on a move.
  rda_viewer* viewer() const;
  SessionReady session() const;
  // Sends bye and tears everything down. Not from on_ended or a viewer callback.
  void Close();

 private:
  RemoteSession() = default;
  void KeepAlive();
  // A session_ready while the session runs: the session moved. Connects the
  // new picture and swaps it in; false if it cannot.
  bool FollowMove(const SessionReady& ready);
  // The viewer's callbacks go through these (user = this) so the session can
  // take the clipboard messages; the rest are passed on to options_.viewer.
  rda_viewer_config ViewerConfig();
  static void OnStreamInfo(void* user, int32_t width, int32_t height);
  static void OnFrame(void* user, const rda_image* bgra, const rda_rect* changed, size_t changed_count,
                      const rda_frame_info* info);
  static void OnCursor(void* user, const rda_cursor_state* state);
  static void OnState(void* user, rda_state state, rda_status reason);
  static void OnMessage(void* user, uint32_t type, const uint8_t* payload, size_t n);

  Options options_;
  std::unique_ptr<ClientConnection> control_;
  mutable std::mutex mu_;  // guards viewer_ and session_ once the session runs
  SessionReady session_;
  rda_viewer* viewer_ = nullptr;
  std::unique_ptr<ClipboardSync> clipboard_;  // outlives viewer_
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace rdm
