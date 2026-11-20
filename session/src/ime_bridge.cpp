#include "rdmachine/session/ime_bridge.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>

#include "rdmachine/agent_link.h"

namespace rdm {

namespace {

std::optional<ImeRange> RangeOf(const nlohmann::json& m, const char* key) {
  auto it = m.find(key);
  if (it == m.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number_unsigned() ||
      !(*it)[1].is_number_unsigned()) {
    return std::nullopt;
  }
  return ImeRange{(*it)[0].get<uint32_t>(), (*it)[1].get<uint32_t>()};
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

bool ImeBridge::Send(const nlohmann::json& m) {
  std::shared_ptr<AgentChannel> ch;
  {
    std::lock_guard lock(mu_);
    ch = channel_;
  }
  return ch && ch->Send(m);
}

void ImeBridge::Mark(uint64_t seq, const std::string& text, uint32_t sel_location, uint32_t sel_length) {
  Send({{"type", "mark"}, {"seq", seq}, {"text", text}, {"sel", {sel_location, sel_length}}});
}

bool ImeBridge::Commit(uint64_t seq, const std::string& text, int timeout_ms) {
  if (!connected_ || !client_active_) return false;
  if (!Send({{"type", "commit"}, {"seq", seq}, {"text", text}})) return false;
  std::unique_lock lock(mu_);
  const bool answered = ack_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                         [&] { return acked_seq_ >= seq || !connected_; });
  if (!answered) {
    if (log_) log_("input method: no answer to a commit within " + std::to_string(timeout_ms) + " ms");
    return false;
  }
  return acked_seq_ == seq && acked_ok_;
}

std::string ImeBridge::client_app() const {
  std::lock_guard lock(mu_);
  return client_app_;
}

void ImeBridge::Cancel(uint64_t seq) { Send({{"type", "cancel"}, {"seq", seq}}); }

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
    {
      std::lock_guard lock(mu_);
      acked_seq_ = m.value("seq", uint64_t{0});
      acked_ok_ = m.value("ok", false);
    }
    ack_cv_.notify_all();
  } else if (type == "client") {
    client_active_ = m.value("active", false);
    {
      std::lock_guard lock(mu_);
      client_app_ = client_active_ ? m.value("app", "") : "";
    }
    if (ev.on_client) ev.on_client(client_active_, m.value("app", ""));
  } else if (type == "state") {
    Report r;
    r.seq = m.value("seq", uint64_t{0});
    r.marked = RangeOf(m, "marked");
    r.selected = RangeOf(m, "sel");
    if (auto it = m.find("rect"); it != m.end() && it->is_array() && it->size() == 4) {
      bool ok = true;
      for (const auto& v : *it) ok = ok && v.is_number();
      if (ok) r.rect = GlobalRect{(*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>(),
                                  (*it)[3].get<double>()};
    }
    if (auto it = m.find("active"); it != m.end() && it->is_boolean()) {
      r.active = it->get<bool>();
      r.app = m.value("app", "");
      // Only the field with the focus says so; an older report of one that
      // lost it does not take the focus away from the one that has it.
      if (*r.active) {
        client_active_ = true;
        std::lock_guard lock(mu_);
        client_app_ = r.app;
      }
    }
    if (ev.on_report) ev.on_report(r);
  } else if (type == "ended") {
    if (ev.on_ended) ev.on_ended(m.value("seq", uint64_t{0}));
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
    }
    connected_ = false;
    client_active_ = false;
    ack_cv_.notify_all();
    if (log_ && !stop_) log_("input method disconnected");
    NotifyConnection(false);
  }
}

}  // namespace rdm
