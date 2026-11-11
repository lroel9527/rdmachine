#include "rdmachine/session/remote_session.h"

#include <chrono>

#include "rdmachine/session/viewer.h"

namespace rdm {

std::function<bool(const std::string&)> TrustWithKnownHosts(
    KnownHosts* known_hosts, std::string key, std::function<bool(const std::string& fingerprint)> ask_first_time,
    std::shared_ptr<bool> changed) {
  return [known_hosts, key = std::move(key), ask = std::move(ask_first_time), changed](const std::string& fp) {
    switch (known_hosts->Check(key, fp)) {
      case KnownHosts::Verdict::kTrusted:
        return true;
      case KnownHosts::Verdict::kChanged:
        if (changed) *changed = true;
        return false;
      case KnownHosts::Verdict::kUnknown:
        break;
    }
    if (!ask || !ask(fp)) return false;
    std::string error;
    known_hosts->Remember(key, fp, &error);  // still connect if saving fails
    return true;
  };
}

namespace {

// Waits until the viewer connects, gives up, or `timeout_ms` passes.
rda_state WaitConnected(rda_viewer* v, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const rda_state s = rda_viewer_get_state(v);
    if (s != RDA_STATE_CONNECTING || std::chrono::steady_clock::now() >= deadline) return s;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

// After session_request on an established connection: the next session_ready.
bool AwaitReady(MessageChannel& ch, int timeout_ms, ConnectResult* result) {
  for (;;) {
    auto r = ch.Receive(timeout_ms);
    if (r.status != MessageChannel::Received::kMessage) {
      result->error = r.status == MessageChannel::Received::kTimeout ? "timeout" : "connection_closed";
      return false;
    }
    if (auto* ping = std::get_if<Ping>(&*r.message)) {
      ch.Send(Pong{ping->id});
      continue;
    }
    if (std::holds_alternative<Pong>(*r.message)) continue;
    if (auto* ready = std::get_if<SessionReady>(&*r.message)) {
      result->session = *ready;
      return true;
    }
    if (auto* err = std::get_if<Error>(&*r.message)) {
      result->error = err->code;
      result->detail = err->message;
    } else {
      result->error = "bad_message";
      result->detail = std::string("unexpected ") + MessageType(*r.message);
    }
    return false;
  }
}

}  // namespace

rda_viewer_config RemoteSession::ViewerConfig() {
  const rda_viewer_config& app = options_.viewer;
  rda_viewer_config c = app;
  c.user = this;
  c.on_stream_info = app.on_stream_info ? &RemoteSession::OnStreamInfo : nullptr;
  c.on_frame = app.on_frame ? &RemoteSession::OnFrame : nullptr;
  c.on_cursor = app.on_cursor ? &RemoteSession::OnCursor : nullptr;
  c.on_state = app.on_state ? &RemoteSession::OnState : nullptr;
  c.on_message = &RemoteSession::OnMessage;
  return c;
}

void RemoteSession::OnStreamInfo(void* user, int32_t width, int32_t height) {
  const auto& app = static_cast<RemoteSession*>(user)->options_.viewer;
  app.on_stream_info(app.user, width, height);
}

void RemoteSession::OnFrame(void* user, const rda_image* bgra, const rda_rect* changed, size_t changed_count,
                            const rda_frame_info* info) {
  const auto& app = static_cast<RemoteSession*>(user)->options_.viewer;
  app.on_frame(app.user, bgra, changed, changed_count, info);
}

void RemoteSession::OnCursor(void* user, const rda_cursor_state* state) {
  const auto& app = static_cast<RemoteSession*>(user)->options_.viewer;
  app.on_cursor(app.user, state);
}

void RemoteSession::OnState(void* user, rda_state state, rda_status reason) {
  const auto& app = static_cast<RemoteSession*>(user)->options_.viewer;
  app.on_state(app.user, state, reason);
}

void RemoteSession::OnMessage(void* user, uint32_t type, const uint8_t* payload, size_t n) {
  auto* self = static_cast<RemoteSession*>(user);
  if (ClipboardSync::IsClipboardMessage(type) && self->clipboard_) {
    self->clipboard_->OnMessage(type, payload, n);
    return;
  }
  const auto& app = self->options_.viewer;
  if (app.on_message) app.on_message(app.user, type, payload, n);
}

std::unique_ptr<RemoteSession> RemoteSession::Start(Options options, ConnectResult* result) {
  *result = ConnectAndStartSession(options.control);
  if (!result->error.empty()) return nullptr;

  std::unique_ptr<RemoteSession> s(new RemoteSession());
  s->options_ = std::move(options);
  s->control_ = std::move(result->connection);
  const int connect_ms = s->options_.control.timeout_ms;
  if (s->options_.clipboard && s->options_.clipboard_mode != ClipboardMode::kOff) {
    ClipboardSync::Options so;
    so.mode = s->options_.clipboard_mode;
    so.fetch_on_announce = s->options_.clipboard_fetch_on_announce;
    so.log = s->options_.log;
    RemoteSession* raw = s.get();
    s->clipboard_ = std::make_unique<ClipboardSync>(
        s->options_.clipboard,
        [raw](uint32_t type, std::string_view payload) {
          return rda_viewer_send_message(raw->viewer_, type, reinterpret_cast<const uint8_t*>(payload.data()),
                                         payload.size()) == RDA_OK;
        },
        so);
  }

  for (int attempt = 0;; ++attempt) {
    s->session_ = result->session;
    std::string error;
    if (CreateSessionViewer(s->options_.control.host, s->session_, s->ViewerConfig(), &s->viewer_, &error) != RDA_OK) {
      result->error = "media_failed";
      result->detail = error;
      return nullptr;
    }
    const bool can_fall_back =
        attempt == 0 && s->session_.transport == MediaTransport::kUdp && s->options_.udp_wait_ms > 0;
    const rda_state st = WaitConnected(s->viewer_, can_fall_back ? s->options_.udp_wait_ms : connect_ms);
    if (st == RDA_STATE_CONNECTED) break;
    rda_viewer_destroy(s->viewer_);
    s->viewer_ = nullptr;
    if (!can_fall_back) {
      result->error = "media_failed";
      result->detail = st == RDA_STATE_DISCONNECTED ? "the picture connection was refused"
                                                    : "the picture connection timed out";
      return nullptr;
    }
    // UDP did not get through: ask for TCP on the same control connection.
    SessionRequest tcp = s->options_.control.request;
    tcp.transport = MediaTransport::kTcp;
    if (!s->control_->channel().Send(tcp)) {
      result->error = "connection_closed";
      return nullptr;
    }
    if (!AwaitReady(s->control_->channel(), connect_ms, result)) return nullptr;
  }
  result->session = s->session_;
  // viewer_ stays the same from here on, so the clipboard thread may use it.
  if (s->clipboard_) s->clipboard_->Start();
  s->thread_ = std::thread([raw = s.get()] { raw->KeepAlive(); });
  return s;
}

void RemoteSession::KeepAlive() {
  using Clock = std::chrono::steady_clock;
  constexpr auto kPingEvery = std::chrono::seconds(15);
  constexpr auto kSilenceLimit = std::chrono::seconds(45);
  MessageChannel& ch = control_->channel();
  auto last_sent = Clock::now(), last_heard = Clock::now();
  int64_t next_ping = 1;
  std::string ended;
  while (!stop_ && ended.empty()) {
    auto r = ch.Receive(250);
    const auto now = Clock::now();
    switch (r.status) {
      case MessageChannel::Received::kMessage:
        last_heard = now;
        if (auto* ping = std::get_if<Ping>(&*r.message)) {
          ch.Send(Pong{ping->id});
          last_sent = now;
        } else if (auto* bye = std::get_if<Bye>(&*r.message)) {
          ended = bye->reason.empty() ? "server_closed" : bye->reason;
        } else if (auto* err = std::get_if<Error>(&*r.message)) {
          ended = err->code.empty() ? "server_closed" : err->code;
        }
        break;
      case MessageChannel::Received::kTimeout:
        break;
      case MessageChannel::Received::kClosed:
      case MessageChannel::Received::kProtocolError:
        ended = "connection_lost";
        break;
    }
    if (!ended.empty() || stop_) break;
    if (now - last_heard >= kSilenceLimit) {
      ended = "connection_lost";
    } else if (rda_viewer_get_state(viewer_) == RDA_STATE_DISCONNECTED) {
      ended = "media_lost";
    } else if (now - last_sent >= kPingEvery) {
      ch.Send(Ping{next_ping++});
      last_sent = now;
    }
  }
  if (!ended.empty() && !stop_ && options_.on_ended) options_.on_ended(ended);
}

void RemoteSession::Close() {
  if (clipboard_) clipboard_->Stop();  // before the viewer it sends through
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (viewer_) {
    rda_viewer_destroy(viewer_);
    viewer_ = nullptr;
  }
  if (control_) {
    control_->Close("user_closed");
    control_.reset();
  }
}

RemoteSession::~RemoteSession() { Close(); }

}  // namespace rdm
