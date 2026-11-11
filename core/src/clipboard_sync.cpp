#include "rdmachine/clipboard_sync.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

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
constexpr uint64_t kCongestionEveryUs = 50'000;
// Not a message type: a promise asking the sync thread to fetch.
constexpr uint32_t kLocalFetchEvent = 0;

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

}  // namespace

// ---- promises ------------------------------------------------------------

struct ClipboardPromise::Hub {
  struct Result {
    bool requested = false;
    bool done = false;
    bool ok = false;
    std::string data;
    uint64_t progress_us = 0;  // last sign of life (steady clock)
  };
  std::mutex mu;
  std::condition_variable cv;
  ClipboardSync* sync = nullptr;  // null once the sync has stopped
  uint64_t announce = 0;          // the peer's current announcement; promises of others are void
  std::map<std::string, Result> results;  // by MIME type
};

ClipboardPromise::ClipboardPromise(std::shared_ptr<Hub> hub, uint64_t announce, std::vector<std::string> formats)
    : hub_(std::move(hub)), announce_(announce), formats_(std::move(formats)) {}

std::optional<std::string> ClipboardPromise::FetchFormat(const std::string& mime, const std::function<void()>& pump,
                                                         int stall_ms) {
  std::unique_lock lock(hub_->mu);
  if (hub_->announce != announce_ || !hub_->sync) return std::nullopt;
  if (!hub_->results[mime].requested) {
    Hub::Result& r = hub_->results[mime];
    r.requested = true;
    r.progress_us = SteadyUs();
    // Under the hub's lock, so the sync cannot go away meanwhile (it clears
    // hub->sync under this lock before it stops).
    hub_->sync->PostLocalFetch(announce_, mime);
  }
  while (true) {
    if (hub_->announce != announce_ || !hub_->sync) return std::nullopt;  // replaced, or the session ended
    Hub::Result& r = hub_->results[mime];
    if (r.done) {
      if (!r.ok) return std::nullopt;
      return r.data;
    }
    if (SteadyUs() - r.progress_us > static_cast<uint64_t>(stall_ms) * 1000) {
      r.done = true;  // later calls give up at once too
      r.ok = false;
      return std::nullopt;
    }
    hub_->cv.wait_for(lock, std::chrono::milliseconds(20));
    if (pump) {
      lock.unlock();
      pump();
      lock.lock();
    }
  }
}

// ---- transfers -----------------------------------------------------------

struct ClipboardSync::Outgoing {
  uint64_t id = 0;
  std::string data;
  uint64_t sent = 0, acked = 0;
  std::deque<std::pair<uint64_t, uint64_t>> marks;  // (sent up to, at)
  TransferPacer pacer;
  uint64_t congestion_seen = 0;
  uint64_t congestion_checked_us = 0;

  explicit Outgoing(const TransferPacer::Options& o) : pacer(o) {}
};

struct ClipboardSync::Incoming {
  uint64_t id = 0;
  std::string mime;
  uint64_t size = 0;
  std::string data;
};

ClipboardSync::ClipboardSync(std::shared_ptr<Clipboard> clipboard, SendFn send, Options options)
    : clipboard_(std::move(clipboard)),
      send_(std::move(send)),
      options_(std::move(options)),
      hub_(std::make_shared<ClipboardPromise::Hub>()) {
  hub_->sync = this;
}

ClipboardSync::~ClipboardSync() { Stop(); }

bool ClipboardSync::IsClipboardMessage(uint32_t type) {
  return type == kClipboardTextMessageType || type == kClipboardHelloMessageType ||
         type == kClipboardOfferMessageType || type == kClipboardDataMessageType ||
         type == kClipboardAckMessageType || type == kClipboardCancelMessageType ||
         type == kClipboardAnnounceMessageType || type == kClipboardRequestMessageType;
}

uint64_t ClipboardSync::Now() const { return options_.clock ? options_.clock() : SteadyUs(); }

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
    // Promises on the clipboard give up from now on.
    std::lock_guard lock(hub_->mu);
    hub_->sync = nullptr;
    hub_->announce = 0;
    hub_->results.clear();
  }
  hub_->cv.notify_all();
  {
    std::lock_guard lock(mu_);
    stop_ = true;
    events_.clear();
    events_bytes_ = 0;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
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

void ClipboardSync::PostLocalFetch(uint64_t announce, const std::string& mime) {
  {
    std::lock_guard lock(mu_);
    if (stop_) return;
    events_.emplace_back(kLocalFetchEvent, std::to_string(announce) + "\n" + mime);
  }
  wake_.notify_all();
}

void ClipboardSync::Begin() {
  seen_count_ = clipboard_->ChangeCount();
  send_pending_ = options_.send_on_start && CanSend(options_.mode);
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
  if (!hello_sent_ && send_(kClipboardHelloMessageType, EncodeClipboardHello({}))) {
    hello_sent_ = true;
    hello_sent_us_ = now;
    hello_now = true;
  }

  if (overflow && incoming_) DropIncoming("error", true, "the peer sent too much at once");
  for (auto& [type, payload] : events) Handle(type, payload, now);

  if (check_clipboard) {
    const int64_t count = clipboard_->ChangeCount();
    if (count != seen_count_) {
      seen_count_ = count;
      if (CanSend(options_.mode)) send_pending_ = true;
      // Copied here: the peer's copy is no longer what a paste here gets.
      DropPromise();
    }
  }

  // Once connected (the hello went out), and once the peer has said whether
  // it speaks this version (or has had time to).
  const bool peer_new = peer_hello_ && peer_hello_->version >= kClipboardVersion;
  if (send_pending_ && hello_sent_ &&
      (peer_new || (!hello_now && now - hello_sent_us_ >= static_cast<uint64_t>(options_.hello_wait_ms) * 1000))) {
    std::optional<ClipboardContent> content = clipboard_->Read();
    if (!content || content->empty() || content == last_synced_) {
      send_pending_ = false;  // nothing (shareable) to announce, or the peer has it already
    } else if (Announce(*content)) {
      send_pending_ = false;
    }
    // Otherwise not connected after all: try again on the next round.
  }

  ServeNextRequest();
  return Pump(Now());
}

void ClipboardSync::Handle(uint32_t type, const std::string& payload, uint64_t now) {
  switch (type) {
    case kLocalFetchEvent: {
      const size_t nl = payload.find('\n');
      if (nl == std::string::npos) return;
      LocalFetch(std::stoull(payload.substr(0, nl)), payload.substr(nl + 1));
      return;
    }
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
      DropPromise();
      WriteToClipboard(ClipboardContent::Text(payload));
      return;
    }
    case kClipboardAnnounceMessageType:
      HandleAnnounce(payload);
      return;
    case kClipboardRequestMessageType:
      HandleRequest(payload, now);
      return;
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
  // (No reading first to compare: the clipboard may hold this side's own
  // promise, and reading that would wait for this very thread.)
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

// ---- copying side ---------------------------------------------------------

bool ClipboardSync::Announce(const ClipboardContent& content) {
  const bool peer_new = peer_hello_ && peer_hello_->version >= kClipboardVersion;
  if (!peer_new) {
    // An older peer cannot ask at paste time: it gets plain text now, or
    // nothing.
    last_synced_ = content;
    if (!options_.push_text_to_older_peers) return true;
    const std::string* text = content.Find(kMimeText);
    if (!text || text->empty() || !IsValidUtf8(*text)) return true;  // nothing it can take
    if (text->size() > options_.max_text_bytes) {
      Log("not sending " + std::to_string(text->size()) + " bytes (limit " + std::to_string(options_.max_text_bytes) + ")");
      return true;
    }
    if (!send_(kClipboardTextMessageType, *text)) {
      last_synced_.reset();
      return false;
    }
    return true;
  }

  ClipboardContent offered;
  for (const auto& f : content.formats) {
    if (f.data.empty() || !KnownMime(f.mime) || offered.Find(f.mime)) continue;
    if (TextMime(f.mime) && !IsValidUtf8(f.data)) continue;
    offered.formats.push_back(f);
  }
  if (offered.empty()) {
    last_synced_ = content;
    return true;
  }
  ClipboardAnnounce a;
  a.id = ++next_id_;
  for (const auto& f : offered.formats) a.formats.push_back(f.mime);
  if (!send_(kClipboardAnnounceMessageType, EncodeClipboardAnnounce(a))) return false;
  // Whatever was being sent for the previous copy is no use any more.
  if (outgoing_) DropOutgoing("superseded", true);
  requests_.clear();
  announced_id_ = a.id;
  announced_ = std::move(offered);
  last_synced_ = content;
  return true;
}

void ClipboardSync::HandleRequest(const std::string& payload, uint64_t now) {
  (void)now;
  auto r = DecodeClipboardRequest(payload);
  if (!r) return;
  requests_.push_back(*r);
  ServeNextRequest();
}

// Answers the peer's requests one at a time: each gets an offer, then its data.
void ClipboardSync::ServeNextRequest() {
  while (!outgoing_ && !requests_.empty()) {
    const ClipboardRequest r = requests_.front();
    requests_.pop_front();
    auto out = std::make_unique<Outgoing>(options_.pacer);
    ClipboardOffer offer;
    offer.id = ++next_id_;
    offer.request = r.id;
    const std::string* data = CanSend(options_.mode) && r.announce == announced_id_ ? announced_.Find(r.format) : nullptr;
    if (!data) {
      offer.skipped = "gone";  // copied over since (or never shared)
    } else if (data->size() > options_.max_format_bytes) {
      Log("not sending " + r.format + " of " + Mib(data->size()) + " (limit " + Mib(options_.max_format_bytes) + ")");
      offer.skipped = "too_large";
    } else {
      offer.format = r.format;
      offer.size = data->size();
      out->id = offer.id;
      out->data = *data;
    }
    if (!send_(kClipboardOfferMessageType, EncodeClipboardOffer(offer))) {
      requests_.clear();  // not connected: the peer's promise gives up by itself
      return;
    }
    if (offer.skipped.empty()) outgoing_ = std::move(out);
  }
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
  while (o.sent < o.data.size()) {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, o.data.size() - o.sent));
    if (!o.pacer.CanSend(now, n, o.sent - o.acked)) {
      if (o.sent - o.acked + n > o.pacer.Window()) return idle;  // waits for acknowledgements
      return o.pacer.NextSendUs(now, n);
    }
    const std::string_view bytes = std::string_view(o.data).substr(o.sent, n);
    if (!send_(kClipboardDataMessageType, EncodeClipboardData({o.id, o.sent}, bytes))) {
      Log("sending stopped: the connection is gone");
      outgoing_.reset();
      return idle;
    }
    o.pacer.OnSent(now, n);
    o.sent += n;
    o.marks.emplace_back(o.sent, now);
    now = Now();
  }
  return idle;  // all sent: acknowledgements wake the thread
}

void ClipboardSync::HandleAck(const std::string& payload, uint64_t now) {
  auto a = DecodeClipboardAck(payload);
  if (!a || !outgoing_ || a->id != outgoing_->id) return;  // a cancelled offer's, late
  Outgoing& o = *outgoing_;
  if (a->bytes > o.sent || a->bytes < o.acked) return;
  uint64_t at = 0;
  while (!o.marks.empty() && o.marks.front().first <= a->bytes) {
    at = o.marks.front().second;
    o.marks.pop_front();
  }
  if (at != 0) o.pacer.OnAck(now, now - at);
  o.acked = a->bytes;
  if (o.acked == o.data.size()) {
    outgoing_.reset();
    ServeNextRequest();
  }
}

void ClipboardSync::DropOutgoing(const std::string& reason, bool tell_peer) {
  if (!outgoing_) return;
  if (tell_peer) send_(kClipboardCancelMessageType, EncodeClipboardCancel({outgoing_->id, true, reason}));
  outgoing_.reset();
}

// ---- pasting side ---------------------------------------------------------

void ClipboardSync::HandleAnnounce(const std::string& payload) {
  auto a = DecodeClipboardAnnounce(payload);
  if (!a) return;
  if (!CanReceive(options_.mode)) return;  // nothing of the peer's is pasted here
  DropPromise();
  // The peer copied something newer than what this side was sending it.
  if (outgoing_) DropOutgoing("superseded", true);
  requests_.clear();
  announced_id_ = 0;

  std::vector<std::string> formats;
  for (const auto& f : a->formats) {
    if (KnownMime(f) && std::find(formats.begin(), formats.end(), f) == formats.end()) formats.push_back(f);
  }
  if (formats.empty()) return;  // nothing that could be pasted here
  peer_announce_ = *a;
  peer_announce_->formats = formats;
  {
    std::lock_guard lock(hub_->mu);
    hub_->announce = a->id;
    hub_->results.clear();
  }
  eager_ = options_.fetch_on_announce;
  if (!eager_) {
    auto promise = std::make_shared<ClipboardPromise>(hub_, a->id, formats);
    if (std::optional<int64_t> written = clipboard_->WritePromise(promise)) {
      seen_count_ = *written;
      send_pending_ = false;
      last_synced_.reset();
      return;
    }
    eager_ = true;  // this clipboard cannot keep promises
  }
  // Everything, now.
  eager_content_ = {};
  for (const auto& f : formats) LocalFetch(a->id, f);
}

void ClipboardSync::DropPromise() {
  if (!peer_announce_) return;
  // Nothing half-fetched is written.
  eager_ = false;
  eager_content_ = {};
  pending_requests_.clear();
  if (incoming_) DropIncoming("superseded", true, "");
  peer_announce_.reset();
  {
    std::lock_guard lock(hub_->mu);
    hub_->announce = 0;
    hub_->results.clear();
  }
  hub_->cv.notify_all();
}

void ClipboardSync::LocalFetch(uint64_t announce, const std::string& mime) {
  if (!peer_announce_ || peer_announce_->id != announce) return;  // the promise is void already
  for (const auto& [id, m] : pending_requests_) {
    if (m == mime) return;  // asked already
  }
  if (incoming_ && incoming_->mime == mime) return;
  ClipboardRequest r{announce, ++next_id_, mime};
  if (!send_(kClipboardRequestMessageType, EncodeClipboardRequest(r))) {
    FinishFetch(mime, false, {}, "not connected");
    return;
  }
  pending_requests_.emplace_back(r.id, mime);
}

void ClipboardSync::FinishFetch(const std::string& mime, bool ok, std::string data, const std::string& why) {
  if (!ok && !why.empty()) Log("cannot paste the peer's " + mime + ": " + why);
  if (eager_) {
    if (ok) eager_content_.formats.push_back({mime, data});
    if (pending_requests_.empty() && !incoming_) {
      eager_ = false;
      // In the order announced, as a promise would offer them.
      ClipboardContent c;
      for (const auto& m : peer_announce_ ? peer_announce_->formats : std::vector<std::string>{}) {
        if (const std::string* d = eager_content_.Find(m)) c.formats.push_back({m, *d});
      }
      eager_content_ = {};
      if (!c.empty()) WriteToClipboard(c);
    }
  }
  {
    std::lock_guard lock(hub_->mu);
    if (peer_announce_ && hub_->announce == peer_announce_->id) {
      auto& r = hub_->results[mime];
      r.done = true;
      r.ok = ok;
      r.data = std::move(data);
    }
  }
  hub_->cv.notify_all();
}

void ClipboardSync::HandleOffer(const std::string& payload) {
  std::optional<ClipboardOffer> offer = DecodeClipboardOffer(payload);
  if (!offer) {
    Log("ignored a malformed offer from the peer");
    return;
  }
  auto refuse = [&](const std::string& reason) {
    send_(kClipboardCancelMessageType, EncodeClipboardCancel({offer->id, false, reason}));
  };
  // Only answers to this side's requests: nothing is taken unasked.
  auto it = std::find_if(pending_requests_.begin(), pending_requests_.end(),
                         [&](const auto& p) { return offer->request != 0 && p.first == offer->request; });
  if (it == pending_requests_.end()) {
    refuse("refused");
    return;
  }
  const std::string mime = it->second;
  pending_requests_.erase(it);
  if (incoming_) DropIncoming("superseded", true, "");
  if (!offer->skipped.empty()) {
    FinishFetch(mime, false, {}, offer->skipped);
    return;
  }
  if (offer->format != mime) {
    refuse("error");
    FinishFetch(mime, false, {}, "the peer sent something else");
    return;
  }
  if (offer->size > options_.max_format_bytes) {
    refuse("too_large");
    FinishFetch(mime, false, {}, Mib(offer->size) + " is over the limit of " + Mib(options_.max_format_bytes));
    return;
  }
  incoming_ = std::make_unique<Incoming>();
  incoming_->id = offer->id;
  incoming_->mime = mime;
  incoming_->size = offer->size;
  incoming_->data.reserve(static_cast<size_t>(offer->size));
  if (offer->size == 0) CompleteIncoming();
}

void ClipboardSync::HandleData(const std::string& payload) {
  std::string_view bytes;
  auto h = DecodeClipboardData(payload, &bytes);
  if (!h || !incoming_ || h->id != incoming_->id) return;  // a cancelled offer's, late
  Incoming& in = *incoming_;
  if (h->offset != in.data.size() || bytes.empty() || bytes.size() > in.size - in.data.size()) {
    DropIncoming("error", true, "the peer sent data out of order");
    return;
  }
  in.data.append(bytes);
  {
    // A paste waiting for this is still getting somewhere.
    std::lock_guard lock(hub_->mu);
    if (auto r = hub_->results.find(in.mime); r != hub_->results.end()) r->second.progress_us = SteadyUs();
  }
  send_(kClipboardAckMessageType, EncodeClipboardAck({in.id, in.data.size()}));
  if (in.data.size() == in.size) CompleteIncoming();
}

void ClipboardSync::CompleteIncoming() {
  std::unique_ptr<Incoming> in = std::move(incoming_);
  if (TextMime(in->mime) && !IsValidUtf8(in->data)) {
    FinishFetch(in->mime, false, {}, "not valid UTF-8");
    return;
  }
  FinishFetch(in->mime, true, std::move(in->data), "");
}

void ClipboardSync::DropIncoming(const std::string& reason, bool tell_peer, const std::string& why) {
  if (!incoming_) return;
  if (tell_peer) send_(kClipboardCancelMessageType, EncodeClipboardCancel({incoming_->id, false, reason}));
  const std::string mime = incoming_->mime;
  incoming_.reset();
  FinishFetch(mime, false, {}, why);
}

void ClipboardSync::HandleCancel(const std::string& payload) {
  auto c = DecodeClipboardCancel(payload);
  if (!c) return;
  if (c->from_sender) {
    // The peer gave up its own offer: ours to receive.
    if (incoming_ && incoming_->id == c->id) DropIncoming(c->reason, false, c->reason == "superseded" ? "" : c->reason);
  } else if (outgoing_ && outgoing_->id == c->id) {
    if (c->reason != "superseded") Log("the peer refused the paste (" + c->reason + ")");
    DropOutgoing(c->reason, false);
  }
}

}  // namespace rdm
