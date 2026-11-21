#include "rdmachine/session/ime_bridge.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>

#include "rdmachine/agent_link.h"

namespace rdm {

namespace {

// Ids remembered for turning a report's "after" back into the client's
// number; a report is never that far behind.
constexpr size_t kRememberedIds = 512;
constexpr size_t kRememberedUnanswered = 16;

std::optional<ImeRange> RangeOf(const nlohmann::json& m, const char* key) {
  auto it = m.find(key);
  if (it == m.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number_unsigned() ||
      !(*it)[1].is_number_unsigned()) {
    return std::nullopt;
  }
  return ImeRange{(*it)[0].get<uint32_t>(), (*it)[1].get<uint32_t>()};
}

std::optional<uint64_t> UnsignedOf(const nlohmann::json& m, const char* key) {
  auto it = m.find(key);
  if (it == m.end() || !it->is_number_unsigned()) return std::nullopt;
  return it->get<uint64_t>();
}

}  // namespace

ImeBridge::ImeBridge(std::string socket_path, TrustFn trust, std::function<void(const std::string&)> log)
    : path_(std::move(socket_path)), trust_(std::move(trust)), log_(std::move(log)) {}

ImeBridge::~ImeBridge() { Stop(); }

bool ImeBridge::Start(std::string* error) {
  if (path_.empty()) {
    *error = "no place for the input method socket";
    return false;
  }
  const std::string dir = path_.substr(0, path_.rfind('/'));
  mkdir(dir.c_str(), 0700);
  listen_fd_ = UnixListen(path_, 0600, error);
  if (listen_fd_ < 0) return false;
  stop_ = false;
  thread_ = std::thread([this] { Loop(); });
  return true;
}

void ImeBridge::Stop() {
  stop_ = true;
  {
    std::lock_guard lock(mu_);
    if (channel_) channel_->Shutdown();
  }
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
    unlink(path_.c_str());
  }
}

void ImeBridge::SetEvents(Events events) {
  std::lock_guard cb(cb_mu_);
  std::lock_guard lock(mu_);
  events_ = std::move(events);
}

void ImeBridge::NewSession() {
  std::lock_guard lock(mu_);
  sent_.clear();
  session_first_id_ = next_id_;
  last_client_seq_ = 0;
  unanswered_.clear();
}

void ImeBridge::NoteClientMessage(uint64_t seq) {
  std::lock_guard lock(mu_);
  last_client_seq_ = seq;
}

std::string ImeBridge::client_app() const {
  std::lock_guard lock(mu_);
  return client_app_;
}

uint64_t ImeBridge::client_ctl() const {
  std::lock_guard lock(mu_);
  return client_ctl_;
}

bool ImeBridge::Send(const nlohmann::json& m) {
  std::shared_ptr<AgentChannel> ch;
  {
    std::lock_guard lock(mu_);
    ch = channel_;
  }
  return ch && ch->Send(m);
}

uint64_t ImeBridge::NextIdLocked(uint64_t seq) {
  const uint64_t id = next_id_++;
  last_client_seq_ = seq;
  sent_.emplace_back(id, seq);
  if (sent_.size() > kRememberedIds) sent_.pop_front();
  return id;
}

// Everything before the first message of this session the input method had
// not put in when it made a report saying `id`; everything so far when it
// had put all of them in. (A report from before this session's first one
// has an id below all of this session's.)
uint64_t ImeBridge::ClientSeqAfterLocked(uint64_t id) const {
  for (const auto& [i, s] : sent_) {
    if (i > id) return s - 1;
  }
  return last_client_seq_;
}

uint64_t ImeBridge::Mark(uint64_t seq, const std::string& text, uint32_t sel_location, uint32_t sel_length) {
  uint64_t id;
  {
    std::lock_guard lock(mu_);
    id = NextIdLocked(seq);
  }
  Send({{"type", "mark"}, {"seq", seq}, {"id", id}, {"text", text}, {"sel", {sel_location, sel_length}}});
  return id;
}

uint64_t ImeBridge::Cancel(uint64_t seq) {
  uint64_t id;
  {
    std::lock_guard lock(mu_);
    id = NextIdLocked(seq);
  }
  Send({{"type", "cancel"}, {"seq", seq}, {"id", id}});
  return id;
}

ImeBridge::CommitResult ImeBridge::Commit(uint64_t seq, const std::string& text, int timeout_ms) {
  if (!connected_ || !client_active_) return CommitResult::kNotTaken;
  uint64_t id;
  {
    std::lock_guard lock(mu_);
    id = NextIdLocked(seq);
    pending_id_ = id;
    pending_seq_ = seq;
    pending_done_ = pending_ok_ = false;
  }
  if (!Send({{"type", "commit"}, {"seq", seq}, {"id", id}, {"text", text}})) {
    std::lock_guard lock(mu_);
    pending_id_ = 0;
    return CommitResult::kNotTaken;
  }
  std::unique_lock lock(mu_);
  // Only the answer to this very commit counts: an answer to an earlier one
  // (of this session or of one before, whose numbers were higher) does not.
  ack_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return pending_done_ || !connected_; });
  const bool done = pending_done_, ok = pending_ok_;
  pending_id_ = 0;
  if (done) return ok ? CommitResult::kInserted : CommitResult::kNotTaken;
  if (!connected_) return CommitResult::kNotTaken;  // it went away: nothing will put it in
  unanswered_.emplace_back(id, text);
  if (unanswered_.size() > kRememberedUnanswered) unanswered_.pop_front();
  lock.unlock();
  if (log_) log_("input method: no answer to a commit within " + std::to_string(timeout_ms) + " ms, left to it");
  return CommitResult::kNoAnswer;
}

void ImeBridge::RequestReport(int delay_ms) {
  if (connected_ && client_active_) Send({{"type", "report"}, {"delay", delay_ms}});
}

void ImeBridge::Handle(const nlohmann::json& m) {
  const std::string type = m.value("type", "");
  std::lock_guard cb(cb_mu_);
  Events ev;
  {
    std::lock_guard lock(mu_);
    ev = events_;
  }
  if (type == "ack") {
    const std::optional<uint64_t> id = UnsignedOf(m, "id");
    const uint64_t seq = m.value("seq", uint64_t{0});
    const bool ok = m.value("ok", false);
    std::optional<std::string> late;
    {
      std::lock_guard lock(mu_);
      // An input method before 6.4.0 answers with the client's number only.
      if (pending_id_ != 0 && (id ? *id == pending_id_ : seq == pending_seq_)) {
        pending_done_ = true;
        pending_ok_ = ok;
      } else if (id && !ok) {
        for (auto it = unanswered_.begin(); it != unanswered_.end(); ++it) {
          if (it->first == *id) {
            late = std::move(it->second);
            unanswered_.erase(it);
            break;
          }
        }
      }
    }
    ack_cv_.notify_all();
    if (late) {
      if (log_) log_("input method: a commit left to it was refused after all, typed now");
      if (ev.on_late_refusal) ev.on_late_refusal(*late);
    }
  } else if (type == "client") {
    const bool active = m.value("active", false);
    const uint64_t ctl = m.value("ctl", uint64_t{0});
    {
      std::lock_guard lock(mu_);
      client_app_ = active ? m.value("app", "") : "";
      client_ctl_ = active ? ctl : 0;
    }
    client_active_ = active;
    if (ev.on_client) ev.on_client(active, m.value("app", ""), ctl);
  } else if (type == "state") {
    Report r;
    r.seq = m.value("seq", uint64_t{0});
    r.marked = RangeOf(m, "marked");
    r.selected = RangeOf(m, "sel");
    r.ctl = m.value("ctl", uint64_t{0});
    if (auto it = m.find("rect"); it != m.end() && it->is_array() && it->size() == 4) {
      bool ok = true;
      for (const auto& v : *it) ok = ok && v.is_number();
      if (ok) r.rect = GlobalRect{(*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>(),
                                  (*it)[3].get<double>()};
    }
    if (auto after = UnsignedOf(m, "after")) {
      std::lock_guard lock(mu_);
      r.answers = ClientSeqAfterLocked(*after);
    }
    if (auto it = m.find("active"); it != m.end() && it->is_boolean()) {
      r.active = it->get<bool>();
      r.app = m.value("app", "");
      // Only the field with the focus says so; an older report of one that
      // lost it does not take the focus away from the one that has it.
      if (*r.active) {
        {
          std::lock_guard lock(mu_);
          client_app_ = r.app;
          if (r.ctl) client_ctl_ = r.ctl;
        }
        client_active_ = true;
      }
    }
    if (ev.on_report) ev.on_report(r);
  } else if (type == "ended") {
    if (ev.on_ended) ev.on_ended(UnsignedOf(m, "id"));
  }
}

void ImeBridge::NotifyConnection(bool connected) {
  std::lock_guard cb(cb_mu_);
  Events ev;
  {
    std::lock_guard lock(mu_);
    ev = events_;
  }
  if (ev.on_connection) ev.on_connection(connected);
}

void ImeBridge::Loop() {
  while (!stop_) {
    std::string err;
    const int fd = UnixAccept(listen_fd_, 250, &err);
    if (fd < 0) continue;
    std::string why;
    if (trust_ && !trust_(fd, &why)) {
      if (log_) log_("input method: connection refused: " + why);
      close(fd);
      continue;
    }
    if (!why.empty() && log_) log_("input method: " + why);
    auto ch = std::make_shared<AgentChannel>(fd);
    {
      std::lock_guard lock(mu_);
      channel_ = ch;
    }
    connected_ = true;
    if (log_) log_("input method connected");
    NotifyConnection(true);
    while (!stop_) {
      AgentChannel::Received in = ch->Receive(250);
      if (in.status == AgentChannel::Received::kTimeout) continue;
      if (in.status != AgentChannel::Received::kMessage) break;
      Handle(in.message);
    }
    {
      std::lock_guard lock(mu_);
      channel_.reset();
      client_app_.clear();
      client_ctl_ = 0;
      unanswered_.clear();
    }
    connected_ = false;
    client_active_ = false;
    ack_cv_.notify_all();
    if (log_ && !stop_) log_("input method disconnected");
    NotifyConnection(false);
  }
}

}  // namespace rdm
