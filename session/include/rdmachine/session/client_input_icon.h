#pragma once
// The input source the client types with, on this Mac's menu bar icon
// (docs/ime.md, 7.1.0). While the client types with its own input method
// this Mac's input source is RDMachine Input, which nobody types with, and
// in full screen the client hides its own menu bar: the input menu in view
// would say RDMachine Input. So the server's menu bar icon carries the
// client's input source icon in a corner.
//
// The client knows each icon by a fingerprint (ImeIconFingerprint) and sends
// the image only when the server asks for it: the server keeps every icon it
// was sent, so each one crosses the network once.
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rdm {

struct ClientInputIcon {
  std::string fingerprint;  // empty: none shown
  std::vector<uint8_t> png;
};

// What the menu bar icon shows now. Thread-safe.
ClientInputIcon CurrentClientInputIcon();
// Shows another one (an empty fingerprint: none). macOS: posts
// kClientInputIconChangedNotification (NSNotificationCenter, main thread)
// when it changes.
void SetClientInputIcon(ClientInputIcon icon);
inline constexpr const char* kClientInputIconChangedNotification = "RDMClientInputIconChanged";

// The icons the client has sent, by fingerprint: in memory, and on disk so
// they outlive the session and the process. The disk is read once, when it
// is made, and written on a queue of its own: Get and Put run on the thread
// that also injects the client's keys, which must never wait for the disk.
// Thread-safe.
class InputIconCache {
 public:
  // dir: where they are kept (made when the first one is), and read now;
  // the oldest go beyond max_files.
  explicit InputIconCache(std::string dir, size_t max_files = 64);
  ~InputIconCache();  // after the writes still queued

  // The PNG with this fingerprint; nullopt when it is not here.
  std::optional<std::vector<uint8_t>> Get(const std::string& fingerprint);
  // Keeps a PNG under its fingerprint (in memory at once, on disk soon);
  // false (nothing kept) when it is not a PNG, too large, or not what the
  // fingerprint says.
  bool Put(const std::string& fingerprint, const std::vector<uint8_t>& png);
  // Returns once what Put queued is on disk (tests).
  void Flush();

 private:
  void Remember(const std::string& fingerprint, std::vector<uint8_t> png);  // with mu_

  std::mutex mu_;
  const std::string dir_;
  const size_t max_files_;
  std::map<std::string, std::vector<uint8_t>> memory_;
  void* queue_ = nullptr;  // the disk's (dispatch_queue_t)
};

// ~/Library/Caches/org.rdmachine.server/input-icons (home from the password
// database). Empty when there is no home.
std::string InputIconCacheDir();

}  // namespace rdm
