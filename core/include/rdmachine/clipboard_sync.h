#pragma once
// Clipboard sharing between the two ends of a session (docs/protocol.md,
// "剪贴板"): text, rich text and images. Platform independent: the system
// clipboard comes in through the Clipboard interface (NSPasteboard on macOS)
// and messages go out through a send function (rdaccel application
// messages, see clipboard_wire.h), so the logic is tested on Linux with an
// in-memory clipboard.
//
// Nothing leaves a computer until the other side pastes it. Each side polls
// its own clipboard (NSPasteboard has no change notification) and, when
// something was copied, only announces which formats it has. The peer puts
// a promise of that on its clipboard (ClipboardPromise); when an application
// there pastes, the promise asks for exactly the format it needs, which then
// comes across. Content that came from the peer is never announced back, so
// the two sides do not ping-pong. Copied files are not shared (only their
// names, as text); a transfer window will carry files.
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "rdmachine/clipboard_wire.h"
#include "rdmachine/transfer_pacer.h"

namespace rdm {

struct ClipboardFormat {
  std::string mime;  // kMimeText, kMimeRtf, kMimeHtml, kMimePng
  std::string data;

  bool operator==(const ClipboardFormat&) const = default;
};

// What is on a clipboard, as far as it is shared.
struct ClipboardContent {
  std::vector<ClipboardFormat> formats;

  bool empty() const { return formats.empty(); }
  const std::string* Find(std::string_view mime) const;
  bool operator==(const ClipboardContent&) const = default;

  static ClipboardContent Text(std::string text);
};

// What the peer copied, as put on this side's clipboard: fetched from the
// peer when an application pastes. Made by ClipboardSync; thread-safe.
class ClipboardPromise {
 public:
  struct Hub;  // shared with the ClipboardSync

  ClipboardPromise(std::shared_ptr<Hub> hub, uint64_t announce, std::vector<std::string> formats);

  const std::vector<std::string>& formats() const { return formats_; }  // MIME types

  // Fetches `mime` from the peer and waits for it, calling `pump` every few
  // milliseconds meanwhile (macOS: runs the main run loop, so the
  // application stays responsive while the paste waits). The first call
  // fetches, later ones return the same result. nullopt when it cannot be
  // had: the peer refused or is gone, a newer copy replaced this one, it is
  // over the limit, or nothing has moved for `stall_ms`.
  std::optional<std::string> FetchFormat(const std::string& mime, const std::function<void()>& pump,
                                         int stall_ms = 30000);

 private:
  std::shared_ptr<Hub> hub_;
  uint64_t announce_;
  std::vector<std::string> formats_;
};

// The system clipboard. Called only from the ClipboardSync's own thread (or
// the caller of PollOnce), never from two threads at once.
class Clipboard {
 public:
  virtual ~Clipboard() = default;
  // A number that changes whenever anyone changes the clipboard
  // (NSPasteboard.changeCount).
  virtual int64_t ChangeCount() = 0;
  // What the clipboard holds in the shared formats; nullopt when it holds
  // something that must not leave this computer (macOS: marked concealed or
  // transient by a password manager). Files copied in a file manager come
  // as their names (text), and nothing else.
  virtual std::optional<ClipboardContent> Read() = 0;
  // Replaces the clipboard's contents and returns the change count that this
  // write produced (NSPasteboard's clearContents returns it), or nullopt on
  // failure. Taking the count from the write itself, rather than reading it
  // afterwards, keeps a copy made right after our write from being mistaken
  // for ours.
  virtual std::optional<int64_t> Write(const ClipboardContent& content) = 0;
  // Replaces the clipboard's contents with a promise of the peer's copy:
  // the formats it announces, fetched through `promise` when an application
  // pastes (from whatever thread the system asks on). Returns the write's
  // change count as Write does. Read() must not fetch it: while the promise
  // is on the clipboard, Read() returns nullopt. nullopt here when this
  // clipboard cannot keep promises (the caller then fetches everything and
  // uses Write).
  virtual std::optional<int64_t> WritePromise(std::shared_ptr<ClipboardPromise> promise) {
    (void)promise;
    return std::nullopt;
  }
};

// Which way the clipboard may go, as the user sets it (NoMachine has the same four).
enum class ClipboardMode {
  kBoth,     // both ways (default)
  kSend,     // only from this side to the peer
  kReceive,  // only from the peer to this side
  kOff,
};

std::optional<ClipboardMode> ParseClipboardMode(std::string_view s);  // "both", "send", "receive", "off"
const char* ClipboardModeName(ClipboardMode m);

// Whether `s` is well-formed UTF-8 (no overlongs, surrogates or code points
// above U+10FFFF).
bool IsValidUtf8(std::string_view s);

class ClipboardSync {
 public:
  // Sends a message to the peer; false if it could not be sent now (for
  // example the media connection is not up yet).
  using SendFn = std::function<bool(uint32_t type, std::string_view payload)>;

  struct Options {
    ClipboardMode mode = ClipboardMode::kBoth;
    // Plain text to and from a peer of an older version, which takes it as
    // one message (rdaccel allows 16 MiB).
    size_t max_text_bytes = 4u << 20;
    // Give plain text to an older peer as soon as it is copied (it cannot
    // ask for it at paste time). The server does, so older clients keep
    // working; the client does not, so nothing it copies goes to an older
    // server.
    bool push_text_to_older_peers = false;
    // Fetch everything the peer announces right away and Write it, instead
    // of a promise (for a clipboard without WritePromise, or a process with
    // no main run loop to answer the system's requests).
    bool fetch_on_announce = false;
    // One format of one copy (a large image, a long text), either way.
    size_t max_format_bytes = 64u << 20;
    int poll_ms = 250;
    // A peer that has not said hello this long after this side's hello is an
    // older version (plain text only); until then a copy waits.
    int hello_wait_ms = 1500;
    // Announce what is on the clipboard when syncing starts. Otherwise only
    // later copies are announced.
    bool send_on_start = false;
    // Cumulative count of picture frames skipped for congestion (the
    // server's rdaccel host knows it); a large paste slows down when it
    // rises.
    std::function<uint64_t()> congestion_events;
    TransferPacer::Options pacer;
    // Microseconds on a monotonic clock; empty = std::chrono::steady_clock
    // (tests drive their own).
    std::function<uint64_t()> clock;
    std::function<void(const std::string&)> log;  // may be empty
  };

  ClipboardSync(std::shared_ptr<Clipboard> clipboard, SendFn send, Options options);
  ~ClipboardSync();  // Stop()

  ClipboardSync(const ClipboardSync&) = delete;
  ClipboardSync& operator=(const ClipboardSync&) = delete;

  // The message types this class takes (route them to OnMessage).
  static bool IsClipboardMessage(uint32_t type);

  // Takes the clipboard's current state as the starting point and starts
  // the sync thread. Call once.
  void Start();
  // Stops and joins the thread. Promises on the clipboard give nothing from
  // then on.
  void Stop();

  // A clipboard message from the peer. Any thread, never blocks on the
  // clipboard: the work is done on the sync thread.
  void OnMessage(uint32_t type, const uint8_t* payload, size_t n);

  // Without Start(): take the starting point / run one round by hand (tests).
  void Begin();
  void PollOnce();

 private:
  friend class ClipboardPromise;
  struct Outgoing;
  struct Incoming;

  void Run();
  // One round: messages from the peer, the clipboard (if check_clipboard),
  // then data as the pacer allows. Returns when to come back (steady clock
  // microseconds).
  uint64_t Step(bool check_clipboard);
  void Handle(uint32_t type, const std::string& payload, uint64_t now);
  // Copying side.
  bool Announce(const ClipboardContent& content);  // false: not connected
  void HandleRequest(const std::string& payload, uint64_t now);
  void ServeNextRequest();
  void HandleAck(const std::string& payload, uint64_t now);
  uint64_t Pump(uint64_t now);
  void DropOutgoing(const std::string& reason, bool tell_peer);
  // Pasting side.
  void HandleAnnounce(const std::string& payload);
  void LocalFetch(uint64_t announce, const std::string& mime);  // a promise wants `mime`
  void FinishFetch(const std::string& mime, bool ok, std::string data, const std::string& why);
  void HandleOffer(const std::string& payload);
  void HandleData(const std::string& payload);
  void CompleteIncoming();
  void DropIncoming(const std::string& reason, bool tell_peer, const std::string& why);
  void DropPromise();  // the peer's copy is no longer on offer here
  void HandleCancel(const std::string& payload);
  void WriteToClipboard(const ClipboardContent& content);
  void Log(const std::string& line);
  uint64_t Now() const;
  // From a promise (any thread): queue a fetch for the sync thread.
  void PostLocalFetch(uint64_t announce, const std::string& mime);

  const std::shared_ptr<Clipboard> clipboard_;
  const SendFn send_;
  const Options options_;

  std::mutex mu_;  // guards events_, events_bytes_, overflow_, stop_
  std::condition_variable wake_;
  std::deque<std::pair<uint32_t, std::string>> events_;
  size_t events_bytes_ = 0;
  bool overflow_ = false;
  bool stop_ = false;
  std::thread thread_;

  // Sync thread only.
  int64_t seen_count_ = 0;
  bool send_pending_ = false;  // the local clipboard changed and is not announced yet
  std::optional<ClipboardContent> last_synced_;  // last content announced or received
  bool hello_sent_ = false;
  uint64_t hello_sent_us_ = 0;
  std::optional<ClipboardHello> peer_hello_;  // nullopt: an older peer (so far)
  uint64_t next_id_ = 0;
  // Copying side: what was announced, and the peer's requests for it that
  // wait for the outgoing slot.
  uint64_t announced_id_ = 0;
  ClipboardContent announced_;
  std::deque<ClipboardRequest> requests_;
  std::unique_ptr<Outgoing> outgoing_;
  // Pasting side: the peer's current announcement, the requests sent for it
  // (request id -> MIME type), and what is coming in.
  std::optional<ClipboardAnnounce> peer_announce_;
  std::shared_ptr<ClipboardPromise::Hub> hub_;  // shared with promises; lives as long as this
  std::vector<std::pair<uint64_t, std::string>> pending_requests_;
  std::unique_ptr<Incoming> incoming_;
  // Fetching the whole announcement at once (options_.fetch_on_announce, or
  // a clipboard that cannot keep promises): what has come so far.
  bool eager_ = false;
  ClipboardContent eager_content_;
  uint64_t next_clipboard_check_us_ = 0;
};

}  // namespace rdm
