#pragma once
// The agent's end of the link to RDMachine Input, the server's input method
// (docs/ime.md, 7): it listens on a Unix socket for the input method process
// of this user, passes the client's composition and committed text to it,
// and hands its reports (which app has the focus, where the composition is)
// to the media host.
//
// Every mark, commit and cancel goes over with an id of the bridge's own,
// rising for as long as the bridge runs (the client's numbers start again
// with every connection). The input method answers a commit with its id and
// says in each report up to which id it had put things in, which the bridge
// turns back into the client's number (6.4.0).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "rdmachine/ime_wire.h"

namespace rdm {

class AgentChannel;

class ImeBridge {
 public:
  // Points, origin top left of the primary display (CoreGraphics' global
  // coordinates).
  struct GlobalRect {
    double x = 0, y = 0, width = 0, height = 0;
  };
  struct Report {
    uint64_t seq = 0;  // the reporting controller's last client message (for the log)
    std::optional<ImeRange> marked, selected;
    std::optional<GlobalRect> rect;  // where the composition starts
    std::optional<bool> active;      // the reporting field has the focus (6.3.6)
    std::string app;
    uint64_t ctl = 0;  // which controller over there, one per text field (6.4.0); 0 = not said
    // The client's highest number this report reflects: everything before
    // the first mark, commit or cancel the input method had not put in yet
    // when it made the report (messages that do not go to it, such as the
    // mode, change nothing over there). nullopt = not said (an input method
    // before 6.4.0).
    std::optional<uint64_t> answers;
  };
  // Called on the bridge's thread, one at a time.
  struct Events {
    std::function<void(bool connected)> on_connection;
    // ctl: which controller (0 = not said).
    std::function<void(bool active, const std::string& app, uint64_t ctl)> on_client;
    std::function<void(const Report&)> on_report;
    // The system ended the composition over there. mark_id: the id Mark()
    // returned for the composition that ended (nullopt = not said).
    std::function<void(std::optional<uint64_t> mark_id)> on_ended;
    // A commit Commit() left to the input method (kNoAnswer) was refused
    // after all: nothing put it in, the caller types it now.
    std::function<void(const std::string& text)> on_late_refusal;
  };
  // trust: whether the process on a connected socket may be the input
  // method; null = anyone of this user (tests).
  using TrustFn = std::function<bool(int fd, std::string* why)>;

  ImeBridge(std::string socket_path, TrustFn trust, std::function<void(const std::string&)> log);
  ~ImeBridge();  // Stop()
  ImeBridge(const ImeBridge&) = delete;
  ImeBridge& operator=(const ImeBridge&) = delete;

  bool Start(std::string* error);
  void Stop();
  // Returns once no earlier event runs any more.
  void SetEvents(Events events);
  // A session starts: the client numbers its messages from 1 again, so
  // reports made before this point answer none of them.
  void NewSession();
  // A client message that does not go to the input method (the mode): a
  // report reflects it once it reflects everything sent before it.
  void NoteClientMessage(uint64_t seq);

  // The input method process is connected, and a text field that takes
  // input methods has the focus (as it last said).
  bool connected() const { return connected_.load(); }
  bool client_active() const { return client_active_.load(); }
  // The app and controller of that text field ("" and 0 when none).
  std::string client_app() const;
  uint64_t client_ctl() const;

  // seq: the client's number. Return the id they went over with.
  uint64_t Mark(uint64_t seq, const std::string& text, uint32_t sel_location, uint32_t sel_length);
  uint64_t Cancel(uint64_t seq);
  enum class CommitResult {
    kInserted,  // the input method put it in
    kNotTaken,  // it is not connected, has no text field, or said no: the caller types it
    kNoAnswer,  // no answer within timeout_ms: it has the text and will put it in (typing it
                // too would put it in twice); on_late_refusal if it says no after all
  };
  CommitResult Commit(uint64_t seq, const std::string& text, int timeout_ms);
  // Asks for a fresh report delay_ms from now (a click or a key may have
  // moved the caret); later requests replace one still waiting.
  void RequestReport(int delay_ms);

 private:
  void Loop();
  void Handle(const nlohmann::json& m);
  void NotifyConnection(bool connected);
  bool Send(const nlohmann::json& m);
  uint64_t NextIdLocked(uint64_t seq);
  uint64_t ClientSeqAfterLocked(uint64_t id) const;

  const std::string path_;
  const TrustFn trust_;
  const std::function<void(const std::string&)> log_;
  int listen_fd_ = -1;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false}, client_active_{false};

  std::mutex cb_mu_;       // held while an event runs, so SetEvents can wait for it
  mutable std::mutex mu_;  // guards the fields below
  std::shared_ptr<AgentChannel> channel_;
  Events events_;
  std::string client_app_;
  uint64_t client_ctl_ = 0;
  uint64_t next_id_ = 1;
  uint64_t session_first_id_ = 1;
  uint64_t last_client_seq_ = 0;  // the client's last number this session
  std::deque<std::pair<uint64_t, uint64_t>> sent_;  // (id, client seq) of this session, oldest first
  // The commit Commit() waits for (0 = none).
  uint64_t pending_id_ = 0, pending_seq_ = 0;
  bool pending_done_ = false, pending_ok_ = false;
  std::condition_variable ack_cv_;
  std::deque<std::pair<uint64_t, std::string>> unanswered_;  // commits left to the input method
};

// macOS: the bridge at ImeSocketPath(), trusting only processes signed with
// this program's certificate. Registers RDMachine Input with Text Input
// Sources when it is installed. Null when the bridge cannot listen.
std::shared_ptr<ImeBridge> SystemImeBridge(std::function<void(const std::string&)> log);

// Whether some process holds secure keyboard input (a password field).
bool SecureInputActive();

// Converts a global rectangle to the pixels of the display it is on:
// that display's origin (global points, as rda_display_info.x/y) and the
// rectangle relative to it in pixels. False when it is on no display.
bool GlobalRectToDisplayPixels(const ImeBridge::GlobalRect& r, int32_t* origin_x, int32_t* origin_y, ImeRect* px);

// Whether a process on a connected Unix socket is signed with the same
// certificate as this one (any identifier: RDMachine Input is another
// bundle than RDMachine Server). True, with *why set, when this program is
// not signed with a certificate (development builds).
bool PeerSignedWithOurCertificate(int fd, std::string* why);

}  // namespace rdm
