#pragma once
// Client side of a whole session: control connection (with the known-hosts
// check), the rdaccel viewer, and keeping the control
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
    ClientOptions control;
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
    // Where files the server copied appear when pasted here (null: as their
    // names), and where they are written on the way (docs/file-paste.md).
    std::shared_ptr<FileTarget> file_target;
    std::string file_staging_dir;
    std::function<void(const std::string&)> log;  // may be empty
    // Sound (docs/audio.md): ask for the server's sound (session_request.
    // audio); when the session has it (session_ready.audio), the main
    // picture's viewer takes it and a player plays it on this computer's
    // default output device, or on none at all with audio_null_device (the
    // command-line client, tests: read and thrown away).
    bool audio = false;
    bool audio_null_device = false;
    // Whether this computer's output is muted or at volume 0 (1), audible (0)
    // or not known (-1): checked about once a second; while it is muted the
    // sound is paused (nobody hears it; docs/audio.md, 5.6). Empty: the
    // player's own read-only check (rda_audio_player_output_muted). Tests
    // set it.
    std::function<int()> output_muted;
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
  // On rdaccel's bulk channel (below the picture), and how much of it waits
  // there (-1: not known).
  rda_status SendBulkMessage(uint32_t type, const uint8_t* payload, size_t n);
  int64_t BulkQueuedBytes() const;
  // Sound: pause it (the user turned it off in the menu): the server sends
  // nothing meanwhile (kAudioControlMessageType). Said again after a move.
  // The sound is also paused while this computer's output is muted (see
  // Options::output_muted). Thread-safe.
  void SetAudioPaused(bool paused);
  // Paused now, for whichever reason.
  bool audio_paused() const { return sent_paused_; }
  // Whether the current session plays sound (a player is running).
  bool audio_playing() const;
  // Whether this session asked for sound (Options::audio), and the protocol
  // revision both sides speak (protocol.h; sound needs 3).
  bool audio_requested() const { return options_.audio; }
  int revision() const { return revision_; }
  // What this session received so far, over all its picture connections
  // (the main one, extra displays, those a move replaced), sound included:
  // rdaccel's payload bytes, without packet headers and resends. Still
  // valid after Close(). Thread-safe.
  uint64_t BytesReceived() const;
  // The current viewer, for statistics and tests; may change on a move.
  rda_viewer* viewer() const;
  SessionReady session() const;
  // Extended desktop (docs/protocol.md, "扩展桌面"): connects the picture
  // of session().extra_displays[i] with `config` (the application's
  // callbacks for that display's window) and waits until it is up. Blocks
  // up to a few seconds: not on the main thread, nor from a callback. A
  // move (on_moved) drops the extra pictures; connect them again from the
  // new session().
  bool ConnectExtra(size_t i, rda_viewer_config config, std::string* error);
  // Input for extra display i (its coordinates are its own picture's).
  rda_status SendExtraInput(size_t i, const rda_input_event& ev);
  void CloseExtras();
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
  std::string MediaHostAddress() const;
  // The player for viewer_ when the session has sound (mu_ held); and the
  // paused state sent to the server.
  void StartPlayer();
  void SendAudioPaused();
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
  void Retire(rda_viewer* v);  // before it is destroyed: keeps its count
  std::atomic<uint64_t> retired_bytes_{0};
  std::vector<rda_viewer*> extras_;  // by extra display; null: not connected (guarded by mu_)
  rda_audio_player* player_ = nullptr;  // plays viewer_'s sound; destroyed before it (guarded by mu_)
  std::atomic<bool> audio_paused_{false};  // what the user asked for (SetAudioPaused)
  std::atomic<bool> output_muted_{false};  // this computer's output is muted (KeepAlive checks)
  std::atomic<bool> sent_paused_{false};   // what the server was last told
  std::mutex pause_mu_;                    // keeps what is sent in order
  // Tells the server when either reason changed what it should do; `again`:
  // say it even if not (a new picture connection after a move).
  void UpdateAudioPaused(bool again);
  void CheckOutputMuted();
  int media_wait_ms_ = 0;  // how long a picture connection gets; longer on a far link (Start)
  int revision_ = 0;     // negotiated at the hello
  std::unique_ptr<ClipboardSync> clipboard_;  // outlives viewer_
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace rdm
