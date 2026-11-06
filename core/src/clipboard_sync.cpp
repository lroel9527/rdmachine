#include "rdmachine/clipboard_sync.h"

#include <chrono>

namespace rdm {

std::optional<ClipboardMode> ParseClipboardMode(std::string_view s) {
  if (s == "both") return ClipboardMode::kBoth;
  if (s == "send") return ClipboardMode::kSend;
  if (s == "receive") return ClipboardMode::kReceive;
  if (s == "off") return ClipboardMode::kOff;
  return std::nullopt;
}

const char* ClipboardModeName(ClipboardMode m) {
  switch (m) {
    case ClipboardMode::kBoth:
      return "both";
    case ClipboardMode::kSend:
      return "send";
    case ClipboardMode::kReceive:
      return "receive";
    case ClipboardMode::kOff:
      return "off";
  }
  return "?";
}

bool IsValidUtf8(std::string_view s) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (n - i < len) return false;
    for (size_t k = 1; k < len; ++k) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    static constexpr uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

namespace {

bool CanSend(ClipboardMode m) { return m == ClipboardMode::kBoth || m == ClipboardMode::kSend; }
bool CanReceive(ClipboardMode m) { return m == ClipboardMode::kBoth || m == ClipboardMode::kReceive; }

}  // namespace

ClipboardSync::ClipboardSync(std::shared_ptr<Clipboard> clipboard, SendFn send, Options options)
    : clipboard_(std::move(clipboard)), send_(std::move(send)), options_(std::move(options)) {}

ClipboardSync::~ClipboardSync() { Stop(); }

void ClipboardSync::Log(const std::string& line) {
  if (options_.log) options_.log("clipboard: " + line);
}

void ClipboardSync::Start() {
  if (options_.mode == ClipboardMode::kOff || !clipboard_ || thread_.joinable()) return;
  Begin();  // here, so a copy made right after Start() counts as a change
  thread_ = std::thread([this] { Run(); });
}

void ClipboardSync::Stop() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
    incoming_.reset();
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void ClipboardSync::OnRemoteText(const uint8_t* bytes, size_t n) {
  if (!CanReceive(options_.mode) || !clipboard_) return;  // dropped by policy, quietly
  if (n == 0) return;
  if (n > options_.max_bytes) {
    Log("ignored " + std::to_string(n) + " bytes from the peer (limit " + std::to_string(options_.max_bytes) + ")");
    return;
  }
  std::string text(reinterpret_cast<const char*>(bytes), n);
  if (!IsValidUtf8(text)) {
    Log("ignored text from the peer that is not valid UTF-8");
    return;
  }
  {
    std::lock_guard lock(mu_);
    if (stop_) return;
    incoming_ = std::move(text);
  }
  wake_.notify_all();
}

void ClipboardSync::Begin() {
  seen_count_ = clipboard_->ChangeCount();
  send_pending_ = options_.send_on_start && CanSend(options_.mode);
}

void ClipboardSync::PollOnce() {
  std::optional<std::string> incoming;
  {
    std::lock_guard lock(mu_);
    incoming.swap(incoming_);
  }

  // The peer's text first: it is the newer change, and writing it bumps the
  // change count, which must not count as a local change.
  if (incoming) {
    if (clipboard_->ReadText() != incoming) {
      if (std::optional<int64_t> written = clipboard_->WriteText(*incoming)) {
        // Changes before ours are overwritten; changes after it show up as
        // a different count below.
        seen_count_ = *written;
        send_pending_ = false;
      } else {
        Log("could not write the peer's text to the clipboard");
      }
    }
    last_synced_ = std::move(incoming);
  }

  const int64_t count = clipboard_->ChangeCount();
  if (count != seen_count_) {
    seen_count_ = count;
    if (CanSend(options_.mode)) send_pending_ = true;
  }
  if (!send_pending_) return;

  std::optional<std::string> text = clipboard_->ReadText();
  if (!text || text->empty() || text == last_synced_) {
    send_pending_ = false;  // nothing (shareable) to send, or the peer has it already
    return;
  }
  if (text->size() > options_.max_bytes) {
    Log("not sending " + std::to_string(text->size()) + " bytes (limit " + std::to_string(options_.max_bytes) + ")");
    send_pending_ = false;
    return;
  }
  if (!IsValidUtf8(*text)) {
    send_pending_ = false;
    return;
  }
  if (send_(*text)) {
    last_synced_ = std::move(text);
    send_pending_ = false;
  }
  // Otherwise not connected yet: try again on the next round.
}

void ClipboardSync::Run() {
  std::unique_lock lock(mu_);
  while (!stop_) {
    lock.unlock();
    PollOnce();
    lock.lock();
    wake_.wait_for(lock, std::chrono::milliseconds(options_.poll_ms), [this] { return stop_ || incoming_; });
  }
}

}  // namespace rdm
