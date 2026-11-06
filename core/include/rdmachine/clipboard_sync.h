#pragma once
// Text clipboard sharing between the two ends of a session (docs/protocol.md,
// "剪贴板"). Platform independent: the system clipboard comes in through the
// Clipboard interface (NSPasteboard on macOS) and text goes out through a
// send function (rdaccel's RDA_MSG_CLIPBOARD_TEXT message), so the logic is
// tested on Linux with an in-memory clipboard.
//
// Each side polls its own clipboard (NSPasteboard has no change
// notification), sends new text to the peer and writes the peer's text into
// its own clipboard. Text that came from the peer is never sent back, so the
// two sides do not ping-pong.
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace rdm {

// The system clipboard. Called only from the ClipboardSync's own thread (or
// the caller of PollOnce), never from two threads at once.
class Clipboard {
 public:
  virtual ~Clipboard() = default;
  // A number that changes whenever anyone changes the clipboard
  // (NSPasteboard.changeCount).
  virtual int64_t ChangeCount() = 0;
  // The clipboard's plain text; nullopt when it holds no text, or text that
  // must not leave this computer (macOS: marked concealed or transient by a
  // password manager).
  virtual std::optional<std::string> ReadText() = 0;
  // Replaces the clipboard's contents with `text` and returns the change
  // count that this write produced (NSPasteboard's clearContents returns it),
  // or nullopt on failure. Taking the count from the write itself, rather
  // than reading it afterwards, keeps a copy made right after our write
  // from being mistaken for ours.
  virtual std::optional<int64_t> WriteText(const std::string& text) = 0;
};

// Which way text may go, as the user sets it (NoMachine has the same four).
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
  // Sends text to the peer; false if it could not be sent now (for example
  // the media connection is not up yet), and it is tried again later.
  using SendFn = std::function<bool(const std::string& text)>;

  struct Options {
    ClipboardMode mode = ClipboardMode::kBoth;
    // Larger text is neither sent nor accepted (rdaccel allows 16 MiB per
    // message; this keeps one copy from hogging the link).
    size_t max_bytes = 4u << 20;
    int poll_ms = 250;
    // Send what is on the clipboard when syncing starts (the client: copy,
    // then connect, then paste over there). Otherwise only later changes go.
    bool send_on_start = false;
    std::function<void(const std::string&)> log;  // may be empty
  };

  ClipboardSync(std::shared_ptr<Clipboard> clipboard, SendFn send, Options options);
  ~ClipboardSync();  // Stop()

  ClipboardSync(const ClipboardSync&) = delete;
  ClipboardSync& operator=(const ClipboardSync&) = delete;

  // Takes the clipboard's current state as the starting point and starts
  // the polling thread. Call once.
  void Start();
  // Stops and joins the thread. Text received but not yet applied is dropped.
  void Stop();

  // Text from the peer. Any thread, never blocks on the clipboard: it is
  // written on the sync thread (if several arrive in between, the newest
  // wins).
  void OnRemoteText(const uint8_t* bytes, size_t n);

  // Without Start(): take the starting point / run one round by hand (tests).
  void Begin();
  void PollOnce();

 private:
  void Run();
  void Log(const std::string& line);

  const std::shared_ptr<Clipboard> clipboard_;
  const SendFn send_;
  const Options options_;

  std::mutex mu_;  // guards incoming_, stop_
  std::condition_variable wake_;
  std::optional<std::string> incoming_;
  bool stop_ = false;
  std::thread thread_;

  // Sync thread only.
  int64_t seen_count_ = 0;
  bool send_pending_ = false;  // the local clipboard changed and is not sent yet
  std::optional<std::string> last_synced_;  // last text sent or received
};

}  // namespace rdm
