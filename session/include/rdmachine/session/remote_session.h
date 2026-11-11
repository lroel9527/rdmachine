#pragma once
// Client side of a whole session: control connection (with the known-hosts
// check), the rdaccel viewer, UDP to TCP fallback, and keeping the control
// connection alive until either side ends the session.
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <rdaccel/rdaccel.h>

#include "rdmachine/clipboard_sync.h"
#include "rdmachine/control_client.h"
#include "rdmachine/session/known_hosts.h"

namespace rdm {

// A trust_server callback backed by known_hosts: a pinned fingerprint must
// match (a changed one is refused and reported through *changed); an unknown
// one is passed to `ask_first_time`, and remembered if it returns true.
std::function<bool(const std::string&)> TrustWithKnownHosts(
    KnownHosts* known_hosts, std::string key, std::function<bool(const std::string& fingerprint)> ask_first_time,
    std::shared_ptr<bool> changed);

class RemoteSession {
 public:
  struct Options {
    ClientOptions control;    // request.transport is where it starts (UDP)
    rda_viewer_config viewer;  // the application's callbacks (rda_viewer_config_default first)
    // Clipboard shared with the server (docs/protocol.md, "剪贴板"); null =
    // not shared. What is on the clipboard at connect time is sent too.
    // Clipboard messages are then not passed to viewer.on_message.
    std::shared_ptr<Clipboard> clipboard;
    ClipboardMode clipboard_mode = ClipboardMode::kBoth;
    // Copied files: at most this much file data per copy, either way (0 =
    // no files); files from the server are put in clipboard_staging_dir
    // (empty = none are taken).
    uint64_t clipboard_file_limit = kDefaultClipboardFileLimit;
    std::string clipboard_staging_dir;
    // Progress of copies with files, for a display (the clipboard's thread).
    std::function<void(const ClipboardTransferStatus&)> on_clipboard_status;
    std::function<void(const std::string&)> log;  // may be empty
    int udp_wait_ms = 4000;    // then fall back to TCP; 0 = no fallback
    // The session ended from the other side or the network: "server_closed",
    // "connection_lost", "media_lost", or the server's bye reason. Runs on
    // the session's own thread: do not destroy the session from it.
    std::function<void(const std::string& reason)> on_ended;
  };

  // Connects and waits until the picture's connection is up. On failure
  // returns null with *result describing why (result->error).
  static std::unique_ptr<RemoteSession> Start(Options options, ConnectResult* result);

  ~RemoteSession();  // Close()

  rda_viewer* viewer() const { return viewer_; }
  const SessionReady& session() const { return session_; }
  // Sends bye and tears everything down. Not from on_ended or a viewer callback.
  void Close();

 private:
  RemoteSession() = default;
  void KeepAlive();
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
  SessionReady session_;
  rda_viewer* viewer_ = nullptr;
  std::unique_ptr<ClipboardSync> clipboard_;  // outlives viewer_
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace rdm
