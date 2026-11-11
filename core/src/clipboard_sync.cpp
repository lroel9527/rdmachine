#include "rdmachine/clipboard_sync.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>

#include "rdmachine/clipboard_files.h"

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

const std::string* ClipboardContent::Find(std::string_view mime) const {
  for (const auto& f : formats) {
    if (f.mime == mime) return &f.data;
  }
  return nullptr;
}

ClipboardContent ClipboardContent::Text(std::string text) {
  ClipboardContent c;
  c.formats.push_back({std::string(kMimeText), std::move(text)});
  return c;
}

namespace {

// Data messages: small enough to interleave with other messages, large
// enough to keep the per-message cost low.
constexpr size_t kChunk = 128 * 1024;
// What the peer may have queued here before the sync thread takes it: its
// window is far smaller; more means a broken peer.
constexpr size_t kMaxQueuedBytes = 64u << 20;
constexpr uint64_t kStatusEveryUs = 200'000;
constexpr uint64_t kCongestionEveryUs = 50'000;

uint64_t SteadyUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool CanSend(ClipboardMode m) { return m == ClipboardMode::kBoth || m == ClipboardMode::kSend; }
bool CanReceive(ClipboardMode m) { return m == ClipboardMode::kBoth || m == ClipboardMode::kReceive; }

bool KnownMime(std::string_view m) { return m == kMimeText || m == kMimeRtf || m == kMimeHtml || m == kMimePng; }
bool TextMime(std::string_view m) { return m == kMimeText || m == kMimeHtml; }

std::string Mib(uint64_t bytes) {
  char buf[32];
  snprintf(buf, sizeof buf, "%.1f MiB", static_cast<double>(bytes) / (1 << 20));
  return buf;
}

std::string FirstRootName(const ClipboardOffer& o) {
  for (const auto& e : o.entries) {
    if (e.path.find('/') == std::string::npos) return e.path;
  }
  return {};
}

}  // namespace

struct ClipboardSync::Outgoing {
  ClipboardOffer offer;
  std::vector<std::string> format_data;  // one per offer.formats
  std::vector<std::string> sources;      // one per file item
  std::vector<uint64_t> sizes;           // items
  size_t item = 0;
  uint64_t offset = 0;  // in `item`
  int fd = -1;          // the file item being read
  uint64_t sent = 0, acked = 0, total = 0;
  std::deque<std::pair<uint64_t, uint64_t>> marks;  // (sent up to, at)
  TransferPacer pacer;
  uint64_t congestion_seen = 0;
  uint64_t congestion_checked_us = 0;
  uint64_t reported_us = 0;

  explicit Outgoing(const TransferPacer::Options& o) : pacer(o) {}
  ~Outgoing() {
    if (fd >= 0) close(fd);
  }
};

struct ClipboardSync::Incoming {
  ClipboardOffer offer;
  std::vector<uint64_t> sizes;
  std::vector<size_t> entry_of;  // per item: its index in offer.entries, or SIZE_MAX for formats
  std::vector<std::string> format_data;
  std::unique_ptr<StagingDir> dir;
  size_t item = 0;
  uint64_t offset = 0;
  bool file_open = false;
  uint64_t written = 0, total = 0;
  uint64_t reported_us = 0;
};

ClipboardSync::ClipboardSync(std::shared_ptr<Clipboard> clipboard, SendFn send, Options options)
    : clipboard_(std::move(clipboard)), send_(std::move(send)), options_(std::move(options)) {}

ClipboardSync::~ClipboardSync() { Stop(); }

bool ClipboardSync::IsClipboardMessage(uint32_t type) {
  return type == kClipboardTextMessageType || type == kClipboardHelloMessageType ||
         type == kClipboardOfferMessageType || type == kClipboardDataMessageType ||
         type == kClipboardAckMessageType || type == kClipboardCancelMessageType;
}

uint64_t ClipboardSync::Now() const { return options_.clock ? options_.clock() : SteadyUs(); }

void ClipboardSync::Log(const std::string& line) {
  if (options_.log) options_.log("clipboard: " + line);
}

void ClipboardSync::Report(const ClipboardTransferStatus& s) {
  if (options_.on_status) options_.on_status(s);
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
    events_.clear();
    events_bytes_ = 0;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  // Nothing half-received stays on disk. (Without a thread, as in tests,
  // the caller's thread owns these; Stop runs there too.)
  if (incoming_ && incoming_->dir) incoming_->dir->Remove();
  incoming_.reset();
  outgoing_.reset();
}

void ClipboardSync::OnMessage(uint32_t type, const uint8_t* payload, size_t n) {
  if (!IsClipboardMessage(type)) return;
  {
    std::lock_guard lock(mu_);
    if (stop_) return;
    if (events_bytes_ + n > kMaxQueuedBytes) {
      overflow_ = true;
      return;
    }
    events_.emplace_back(type, std::string(reinterpret_cast<const char*>(payload), n));
    events_bytes_ += n;
  }
  wake_.notify_all();
}

void ClipboardSync::Begin() {
  seen_count_ = clipboard_->ChangeCount();
  send_pending_ = options_.send_on_start && CanSend(options_.mode);
  if (!options_.staging_dir.empty()) {
    // Files received in earlier sessions, except what is still on the
    // clipboard (it may yet be pasted).
    std::string keep;
    if (std::optional<ClipboardContent> c = clipboard_->Read(); c && !c->files.empty()) {
      keep = StagingDirOf(options_.staging_dir, c->files.front());
    }
    RemoveStagingDirs(options_.staging_dir, keep);
  }
}

void ClipboardSync::PollOnce() { Step(/*check_clipboard=*/true); }

void ClipboardSync::Run() {
  std::unique_lock lock(mu_);
  while (!stop_) {
    lock.unlock();
    const uint64_t now = Now();
    const bool check = now >= next_clipboard_check_us_;
    if (check) next_clipboard_check_us_ = now + static_cast<uint64_t>(options_.poll_ms) * 1000;
    const uint64_t wake = std::min(Step(check), next_clipboard_check_us_);
    lock.lock();
    const uint64_t after = Now();
    if (wake > after) {
      wake_.wait_for(lock, std::chrono::microseconds(wake - after), [this] { return stop_ || !events_.empty(); });
    }
  }
}

uint64_t ClipboardSync::Step(bool check_clipboard) {
  std::deque<std::pair<uint32_t, std::string>> events;
  bool overflow;
  {
    std::lock_guard lock(mu_);
    events.swap(events_);
    events_bytes_ = 0;
    overflow = overflow_;
    overflow_ = false;
  }
  uint64_t now = Now();

  // The hello goes first: it tells the peer how to talk to this side.
  bool hello_now = false;
  if (!hello_sent_) {
    ClipboardHello h;
    h.max_file_bytes = CanReceive(options_.mode) && !options_.staging_dir.empty() ? options_.max_file_bytes : 0;
    if (send_(kClipboardHelloMessageType, EncodeClipboardHello(h))) {
      hello_sent_ = true;
      hello_sent_us_ = now;
      hello_now = true;
    }
  }

  if (overflow && incoming_) DropIncoming("error", true, ClipboardTransferStatus::State::kFailed, "the peer sent too much at once");
  for (auto& [type, payload] : events) Handle(type, payload, now);

  if (check_clipboard) {
    const int64_t count = clipboard_->ChangeCount();
    if (count != seen_count_) {
      seen_count_ = count;
      if (CanSend(options_.mode)) send_pending_ = true;
      // Copied here while the peer's copy was on its way: this one is newer.
      if (incoming_) DropIncoming("superseded", true, ClipboardTransferStatus::State::kCancelled, "superseded");
    }
  }

  // Once connected (the hello went out), and once the peer has said whether
  // it speaks version 1 (or has had time to).
  if (send_pending_ && hello_sent_ &&
      (peer_hello_ || (!hello_now && now - hello_sent_us_ >= static_cast<uint64_t>(options_.hello_wait_ms) * 1000))) {
    std::optional<ClipboardContent> content = clipboard_->Read();
    if (!content || content->empty() || content == last_synced_) {
      send_pending_ = false;  // nothing (shareable) to send, or the peer has it already
    } else if (StartSending(*content, now)) {
      send_pending_ = false;
    }
    // Otherwise not connected after all: try again on the next round.
  }

  now = Now();
  return Pump(now);
}

void ClipboardSync::Handle(uint32_t type, const std::string& payload, uint64_t now) {
  switch (type) {
    case kClipboardHelloMessageType:
      if (auto h = DecodeClipboardHello(payload)) {
        if (!peer_hello_) Log("peer speaks clipboard version " + std::to_string(h->version));
        peer_hello_ = *h;
      }
      return;
    case kClipboardTextMessageType: {
      // An older peer's plain text.
      if (!CanReceive(options_.mode)) return;  // dropped by policy, quietly
      if (payload.empty()) return;
      if (payload.size() > options_.max_text_bytes) {
        Log("ignored " + std::to_string(payload.size()) + " bytes from the peer (limit " +
            std::to_string(options_.max_text_bytes) + ")");
        return;
      }
      if (!IsValidUtf8(payload)) {
        Log("ignored text from the peer that is not valid UTF-8");
        return;
      }
      if (incoming_) DropIncoming("superseded", false, ClipboardTransferStatus::State::kCancelled, "superseded");
      WriteToClipboard(ClipboardContent::Text(payload));
      return;
    }
    case kClipboardOfferMessageType:
      HandleOffer(payload);
      return;
    case kClipboardDataMessageType:
      HandleData(payload);
      return;
    case kClipboardAckMessageType:
      HandleAck(payload, now);
      return;
    case kClipboardCancelMessageType:
      HandleCancel(payload);
      return;
  }
}

void ClipboardSync::WriteToClipboard(const ClipboardContent& content) {
  if (clipboard_->Read() == content) {
    last_synced_ = content;
    return;
  }
  if (std::optional<int64_t> written = clipboard_->Write(content)) {
    // Changes before ours are overwritten; changes after it show up as a
    // different count.
    seen_count_ = *written;
    send_pending_ = false;
  } else {
    Log("could not write the peer's copy to the clipboard");
  }
  last_synced_ = content;
}

// ---- sending ------------------------------------------------------------

bool ClipboardSync::StartSending(const ClipboardContent& content, uint64_t now) {
  if (outgoing_) DropOutgoing("superseded", true, ClipboardTransferStatus::State::kCancelled, "superseded");

  if (!peer_hello_) {
    // An older peer: plain text only, in one message.
    const std::string* text = content.Find(kMimeText);
    if (!text || text->empty() || !IsValidUtf8(*text)) return true;  // nothing it can take
    if (text->size() > options_.max_text_bytes) {
      Log("not sending " + std::to_string(text->size()) + " bytes (limit " + std::to_string(options_.max_text_bytes) + ")");
      return true;
    }
    if (!send_(kClipboardTextMessageType, *text)) return false;
    last_synced_ = content;
    return true;
  }

  auto out = std::make_unique<Outgoing>(options_.pacer);
  ClipboardOffer& offer = out->offer;
  offer.id = ++next_id_;
  // Formats: with files only their names as text (a file manager's copy
  // also carries icons and such, which the files make pointless).
  size_t format_bytes = 0;
  for (const auto& f : content.formats) {
    if (f.data.empty() || !KnownMime(f.mime) || offer.formats.size() >= 8) continue;
    if (!content.files.empty() && f.mime != kMimeText) continue;
    if (TextMime(f.mime) && !IsValidUtf8(f.data)) continue;
    if (format_bytes + f.data.size() > options_.max_format_bytes) {
      Log("not sending " + f.mime + " of " + Mib(f.data.size()) + " (limit " + Mib(options_.max_format_bytes) + ")");
      continue;
    }
    format_bytes += f.data.size();
    offer.formats.push_back({f.mime, f.data.size()});
    out->format_data.push_back(f.data);
  }
  uint64_t limit = 0;
  if (!content.files.empty()) {
    limit = std::min(options_.max_file_bytes, peer_hello_->max_file_bytes);
    FileListLimits fl;
    fl.max_bytes = limit;
    fl.max_entries = options_.max_file_entries;
    std::string error;
    if (!ListFiles(content.files, fl, &offer, &out->sources, &error)) Log(error);
    if (!offer.skipped_reason.empty()) offer.skipped_limit = limit;
  }
  if (offer.formats.empty() && offer.entries.empty() && offer.skipped_files == 0) {
    last_synced_ = content;
    return true;  // nothing left to offer
  }
  if (!send_(kClipboardOfferMessageType, EncodeClipboardOffer(offer))) return false;
  last_synced_ = content;

  if (offer.skipped_files > 0 || !offer.skipped_reason.empty()) {
    ClipboardTransferStatus s;
    s.direction = ClipboardTransferStatus::Direction::kSending;
    s.state = ClipboardTransferStatus::State::kSkipped;
    s.files = offer.skipped_files;
    s.detail = offer.skipped_reason;
    s.skipped_bytes = offer.skipped_bytes;
    s.limit_bytes = limit;
    std::filesystem::path first(content.files.front());
    s.name = first.filename().string();
    Log("files not sent (" + s.detail + "): " + std::to_string(s.files) + " files, " + Mib(s.skipped_bytes) +
        " or more; limit " + Mib(limit));
    Report(s);
  }
  out->sizes = offer.ItemSizes();
  out->total = offer.TotalBytes();
  if (out->total == 0 && offer.entries.empty()) return true;  // all said in the offer
  if (out->total == 0) {
    // Only empty files and directories: the offer is all there is.
    outgoing_ = std::move(out);
    ReportOutgoing(ClipboardTransferStatus::State::kDone, "", now, true);
    outgoing_.reset();
    return true;
  }
  if (offer.FileCount() > 0) Log("sending " + std::to_string(offer.FileCount()) + " files, " + Mib(out->total));
  outgoing_ = std::move(out);
  ReportOutgoing(ClipboardTransferStatus::State::kRunning, "", now, true);
  return true;
}

uint64_t ClipboardSync::Pump(uint64_t now) {
  const uint64_t idle = now + 1'000'000;
  if (!outgoing_) return idle;
  Outgoing& o = *outgoing_;
  if (options_.congestion_events && now - o.congestion_checked_us >= kCongestionEveryUs) {
    o.congestion_checked_us = now;
    const uint64_t c = options_.congestion_events();
    if (o.congestion_seen != 0 && c > o.congestion_seen) o.pacer.OnCongestion(now);
    o.congestion_seen = std::max<uint64_t>(c, 1);
  }
  std::string buf;
  while (o.item < o.sizes.size()) {
    const uint64_t remaining = o.sizes[o.item] - o.offset;
    if (remaining == 0) {
      if (o.fd >= 0) {
        close(o.fd);
        o.fd = -1;
      }
      ++o.item;
      o.offset = 0;
      continue;
    }
    const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, remaining));
    if (!o.pacer.CanSend(now, n, o.sent - o.acked)) {
      if (o.sent - o.acked + n > o.pacer.Window()) return idle;  // waits for acknowledgements
      return o.pacer.NextSendUs(now, n);
    }
    std::string_view bytes;
    const size_t formats = o.offer.formats.size();
    if (o.item < formats) {
      bytes = std::string_view(o.format_data[o.item]).substr(o.offset, n);
    } else {
      const std::string& src = o.sources[o.item - formats];
      if (o.fd < 0) {
        o.fd = open(src.c_str(), O_RDONLY | O_CLOEXEC);
        if (o.fd < 0) {
          DropOutgoing("error", true, ClipboardTransferStatus::State::kFailed,
                       "cannot read " + src + ": " + std::strerror(errno));
          return idle;
        }
      }
      buf.resize(n);
      size_t got = 0;
      while (got < n) {
        const ssize_t r = pread(o.fd, buf.data() + got, n - got, static_cast<off_t>(o.offset + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += static_cast<size_t>(r);
      }
      if (got < n) {
        DropOutgoing("error", true, ClipboardTransferStatus::State::kFailed, src + " changed while it was being copied");
        return idle;
      }
      bytes = buf;
    }
    if (!send_(kClipboardDataMessageType, EncodeClipboardData({o.offer.id, static_cast<uint32_t>(o.item), o.offset}, bytes))) {
      DropOutgoing("", false, ClipboardTransferStatus::State::kFailed, "the connection is gone");
      return idle;
    }
    o.pacer.OnSent(now, n);
    o.sent += n;
    o.offset += n;
    o.marks.emplace_back(o.sent, now);
    now = Now();
  }
  return idle;  // all sent: acknowledgements wake the thread
}

void ClipboardSync::HandleAck(const std::string& payload, uint64_t now) {
  auto a = DecodeClipboardAck(payload);
  if (!a || !outgoing_ || a->id != outgoing_->offer.id) return;  // a cancelled offer's, late
  Outgoing& o = *outgoing_;
  if (a->bytes > o.sent || a->bytes < o.acked) return;
  uint64_t at = 0;
  while (!o.marks.empty() && o.marks.front().first <= a->bytes) {
    at = o.marks.front().second;
    o.marks.pop_front();
  }
  if (at != 0) o.pacer.OnAck(now, now - at);
  o.acked = a->bytes;
  if (o.acked == o.total) {
    if (o.offer.FileCount() > 0) Log("sent " + std::to_string(o.offer.FileCount()) + " files, " + Mib(o.total));
    ReportOutgoing(ClipboardTransferStatus::State::kDone, "", now, true);
    outgoing_.reset();
    return;
  }
  ReportOutgoing(ClipboardTransferStatus::State::kRunning, "", now, false);
}

void ClipboardSync::ReportOutgoing(ClipboardTransferStatus::State state, const std::string& detail, uint64_t now,
                                   bool force) {
  if (!outgoing_ || outgoing_->offer.entries.empty()) return;  // only copies with files are shown
  Outgoing& o = *outgoing_;
  if (!force && now - o.reported_us < kStatusEveryUs) return;
  o.reported_us = now;
  ClipboardTransferStatus s;
  s.direction = ClipboardTransferStatus::Direction::kSending;
  s.state = state;
  s.bytes_done = o.acked;
  s.bytes_total = o.total;
  s.files = o.offer.FileCount();
  s.name = FirstRootName(o.offer);
  s.detail = detail;
  Report(s);
}

void ClipboardSync::DropOutgoing(const std::string& reason, bool tell_peer, ClipboardTransferStatus::State state,
                                 const std::string& detail) {
  if (!outgoing_) return;
  if (tell_peer) send_(kClipboardCancelMessageType, EncodeClipboardCancel({outgoing_->offer.id, true, reason}));
  if (state == ClipboardTransferStatus::State::kFailed) Log("sending failed: " + detail);
  if (reason != "superseded") ReportOutgoing(state, detail, Now(), true);
  outgoing_.reset();
}

// ---- receiving ----------------------------------------------------------

void ClipboardSync::HandleOffer(const std::string& payload) {
  std::optional<ClipboardOffer> offer = DecodeClipboardOffer(payload);
  if (!offer) {
    Log("ignored a malformed or unsafe offer from the peer");
    return;
  }
  auto refuse = [&](const std::string& reason) {
    send_(kClipboardCancelMessageType, EncodeClipboardCancel({offer->id, false, reason}));
  };
  if (!CanReceive(options_.mode)) {
    refuse("refused");  // dropped by policy
    return;
  }
  // The peer's copy is newer than whatever was on its way.
  if (incoming_) DropIncoming("superseded", false, ClipboardTransferStatus::State::kCancelled, "superseded");
  if (outgoing_) DropOutgoing("superseded", true, ClipboardTransferStatus::State::kCancelled, "superseded");

  uint64_t format_bytes = 0;
  for (const auto& f : offer->formats) format_bytes += f.size;
  if (format_bytes > options_.max_format_bytes) {
    Log("refused " + Mib(format_bytes) + " of formats from the peer (limit " + Mib(options_.max_format_bytes) + ")");
    refuse("too_large");
    return;
  }
  if (!offer->entries.empty()) {
    if (options_.staging_dir.empty() || options_.max_file_bytes == 0) {
      refuse("refused");
      return;
    }
    if (offer->FileBytes() > options_.max_file_bytes || offer->entries.size() > options_.max_file_entries * 10) {
      Log("refused " + Mib(offer->FileBytes()) + " of files from the peer (limit " + Mib(options_.max_file_bytes) + ")");
      refuse("too_large");
      return;
    }
  }
  if (offer->skipped_files > 0 || !offer->skipped_reason.empty()) {
    ClipboardTransferStatus s;
    s.direction = ClipboardTransferStatus::Direction::kReceiving;
    s.state = ClipboardTransferStatus::State::kSkipped;
    s.files = offer->skipped_files;
    s.detail = offer->skipped_reason;
    s.skipped_bytes = offer->skipped_bytes;
    s.limit_bytes = offer->skipped_limit;
    Log("the peer copied files that were not sent (" + s.detail + "): " + std::to_string(s.files) + " files, " +
        Mib(s.skipped_bytes) + " or more");
    Report(s);
  }
  if (offer->formats.empty() && offer->entries.empty()) return;  // nothing to put on the clipboard

  auto in = std::make_unique<Incoming>();
  in->offer = *offer;
  in->sizes = offer->ItemSizes();
  in->total = offer->TotalBytes();
  in->format_data.resize(offer->formats.size());
  for (size_t i = 0; i < offer->formats.size(); ++i) {
    in->entry_of.push_back(SIZE_MAX);
    in->format_data[i].reserve(static_cast<size_t>(offer->formats[i].size));
  }
  for (size_t i = 0; i < offer->entries.size(); ++i) {
    if (offer->entries[i].kind == OfferEntry::Kind::kFile) in->entry_of.push_back(i);
  }
  if (!offer->entries.empty()) {
    std::string error;
    in->dir = StagingDir::Create(options_.staging_dir, *offer, &error);
    if (!in->dir) {
      Log("cannot take the peer's files: " + error);
      refuse("error");
      ClipboardTransferStatus s;
      s.direction = ClipboardTransferStatus::Direction::kReceiving;
      s.state = ClipboardTransferStatus::State::kFailed;
      s.files = offer->FileCount();
      s.name = FirstRootName(*offer);
      s.detail = error;
      Report(s);
      return;
    }
    Log("receiving " + std::to_string(offer->FileCount()) + " files, " + Mib(in->total));
  }
  incoming_ = std::move(in);
  ReportIncoming(ClipboardTransferStatus::State::kRunning, "", Now(), true);
  AdvanceIncoming();
}

// Past items with no bytes (empty files are created here); completes the
// offer after the last item.
void ClipboardSync::AdvanceIncoming() {
  while (incoming_ && incoming_->item < incoming_->sizes.size() && incoming_->sizes[incoming_->item] == 0) {
    Incoming& in = *incoming_;
    const size_t entry = in.entry_of[in.item];
    if (entry != SIZE_MAX) {
      std::string error;
      if (!in.dir->Open(entry, &error) || !in.dir->Close(&error)) {
        DropIncoming("error", true, ClipboardTransferStatus::State::kFailed, error);
        return;
      }
    }
    ++in.item;
    in.offset = 0;
  }
  if (incoming_ && incoming_->item == incoming_->sizes.size()) CompleteIncoming();
}

void ClipboardSync::HandleData(const std::string& payload) {
  std::string_view bytes;
  auto h = DecodeClipboardData(payload, &bytes);
  if (!h || !incoming_ || h->id != incoming_->offer.id) return;  // a cancelled offer's, late
  Incoming& in = *incoming_;
  if (h->item != in.item || h->offset != in.offset || in.item >= in.sizes.size() ||
      bytes.size() > in.sizes[in.item] - in.offset || bytes.empty()) {
    DropIncoming("error", true, ClipboardTransferStatus::State::kFailed, "the peer sent data out of order");
    return;
  }
  const size_t entry = in.entry_of[in.item];
  std::string error;
  if (entry == SIZE_MAX) {
    in.format_data[in.item].append(bytes);
  } else {
    if (!in.file_open) {
      if (!in.dir->Open(entry, &error)) {
        DropIncoming("error", true, ClipboardTransferStatus::State::kFailed, error);
        return;
      }
      in.file_open = true;
    }
    if (!in.dir->Append(bytes, &error)) {
      DropIncoming("error", true, ClipboardTransferStatus::State::kFailed, error);
      return;
    }
  }
  in.offset += bytes.size();
  in.written += bytes.size();
  if (in.offset == in.sizes[in.item]) {
    if (in.file_open) {
      in.file_open = false;
      if (!in.dir->Close(&error)) {
        DropIncoming("error", true, ClipboardTransferStatus::State::kFailed, error);
        return;
      }
    }
    ++in.item;
    in.offset = 0;
  }
  // Written (to memory or disk) before it is acknowledged: a slow disk
  // slows the sender down.
  send_(kClipboardAckMessageType, EncodeClipboardAck({in.offer.id, in.written}));
  ReportIncoming(ClipboardTransferStatus::State::kRunning, "", Now(), false);
  AdvanceIncoming();
}

void ClipboardSync::CompleteIncoming() {
  Incoming& in = *incoming_;
  ClipboardContent content;
  for (size_t i = 0; i < in.offer.formats.size(); ++i) {
    const std::string& mime = in.offer.formats[i].mime;
    if (!KnownMime(mime)) continue;  // a later version's format
    if (TextMime(mime) && !IsValidUtf8(in.format_data[i])) {
      Log("ignored " + mime + " from the peer that is not valid UTF-8");
      continue;
    }
    content.formats.push_back({mime, std::move(in.format_data[i])});
  }
  if (in.dir) {
    in.dir->Finish();
    content.files = in.dir->RootPaths();
  }
  if (!content.empty()) WriteToClipboard(content);
  if (in.dir) {
    // Earlier copies are no longer on the clipboard.
    RemoveStagingDirs(options_.staging_dir, in.dir->path());
    Log("received " + std::to_string(in.offer.FileCount()) + " files, " + Mib(in.total));
  }
  ReportIncoming(ClipboardTransferStatus::State::kDone, "", Now(), true);
  incoming_.reset();
}

void ClipboardSync::ReportIncoming(ClipboardTransferStatus::State state, const std::string& detail, uint64_t now,
                                   bool force) {
  if (!incoming_ || incoming_->offer.entries.empty()) return;  // only copies with files are shown
  Incoming& in = *incoming_;
  if (!force && now - in.reported_us < kStatusEveryUs) return;
  in.reported_us = now;
  ClipboardTransferStatus s;
  s.direction = ClipboardTransferStatus::Direction::kReceiving;
  s.state = state;
  s.bytes_done = in.written;
  s.bytes_total = in.total;
  s.files = in.offer.FileCount();
  s.name = FirstRootName(in.offer);
  s.detail = detail;
  Report(s);
}

void ClipboardSync::DropIncoming(const std::string& reason, bool tell_peer, ClipboardTransferStatus::State state,
                                 const std::string& detail) {
  if (!incoming_) return;
  if (tell_peer) send_(kClipboardCancelMessageType, EncodeClipboardCancel({incoming_->offer.id, false, reason}));
  if (state == ClipboardTransferStatus::State::kFailed) Log("receiving failed: " + detail);
  if (reason != "superseded") ReportIncoming(state, detail, Now(), true);
  if (incoming_->dir) incoming_->dir->Remove();
  incoming_.reset();
}

void ClipboardSync::HandleCancel(const std::string& payload) {
  auto c = DecodeClipboardCancel(payload);
  if (!c) return;
  if (c->from_sender) {
    // The peer gave up its own offer: ours to receive.
    if (incoming_ && incoming_->offer.id == c->id) {
      DropIncoming(c->reason, false,
                   c->reason == "error" ? ClipboardTransferStatus::State::kFailed : ClipboardTransferStatus::State::kCancelled,
                   c->reason == "error" ? "the other side could not read the files" : c->reason);
    }
  } else if (outgoing_ && outgoing_->offer.id == c->id) {
    Log("the peer refused the copy (" + c->reason + ")");
    DropOutgoing(c->reason == "superseded" ? "superseded" : c->reason, false,
                 c->reason == "error" ? ClipboardTransferStatus::State::kFailed : ClipboardTransferStatus::State::kCancelled,
                 c->reason == "error" ? "the other side could not save the files" : c->reason);
  }
}

}  // namespace rdm
