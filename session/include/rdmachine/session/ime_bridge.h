#pragma once
// The agent's end of the link to RDMachine Input, the server's input method
// (docs/ime.md, 7): it listens on a Unix socket for the input method process
// of this user, passes the client's composition and committed text to it,
// and hands its reports (which app has the focus, where the composition is)
// to the media host.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

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
    uint64_t seq = 0;
    std::optional<ImeRange> marked, selected;
    std::optional<GlobalRect> rect;  // where the composition starts
    std::optional<bool> active;      // the reporting field has the focus (6.3.6)
    std::string app;
  };
  // Called on the bridge's thread, one at a time.
  struct Events {
    std::function<void(bool connected)> on_connection;
    std::function<void(bool active, const std::string& app)> on_client;
    std::function<void(const Report&)> on_report;
    std::function<void(uint64_t seq)> on_ended;  // the system ended the composition over there
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

  // The input method process is connected, and a text field that takes
  // input methods has the focus (as it last said).
  bool connected() const { return connected_.load(); }
  bool client_active() const { return client_active_.load(); }
  // The app of that text field ("" when none).
  std::string client_app() const;

  void Mark(uint64_t seq, const std::string& text, uint32_t sel_location, uint32_t sel_length);
  // Waits up to timeout_ms for the input method to say it put the text in.
  // False when it is not connected, has no text field, or did not answer in
  // time: the caller types the text some other way.
  bool Commit(uint64_t seq, const std::string& text, int timeout_ms);
  void Cancel(uint64_t seq);
  // Asks for a fresh report delay_ms from now (a click or a key may have
  // moved the caret); later requests replace one still waiting.
  void RequestReport(int delay_ms);

 private:
  void Loop();
  void Handle(const nlohmann::json& m);
  void NotifyConnection(bool connected);
  bool Send(const nlohmann::json& m);

  const std::string path_;
  const TrustFn trust_;
  const std::function<void(const std::string&)> log_;
  int listen_fd_ = -1;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false}, client_active_{false};

  std::mutex cb_mu_;  // held while an event runs, so SetEvents can wait for it
  mutable std::mutex mu_;  // guards the fields below
  std::shared_ptr<AgentChannel> channel_;
  Events events_;
  std::string client_app_;
  uint64_t acked_seq_ = 0;
  bool acked_ok_ = false;
  std::condition_variable ack_cv_;
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
