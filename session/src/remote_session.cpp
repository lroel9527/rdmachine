#include "rdmachine/session/remote_session.h"

#include <chrono>

#include "rdmachine/session/viewer.h"

namespace rdm {

std::function<bool(const std::string&)> TrustWithKnownHosts(
    KnownHosts* known_hosts, std::string key, std::function<bool(const std::string& fingerprint)> ask_first_time,
    std::shared_ptr<bool> changed,
    std::function<bool(const std::string& old_fingerprint, const std::string& new_fingerprint)> ask_changed) {
  return [known_hosts, key = std::move(key), ask = std::move(ask_first_time), changed,
          ask_changed = std::move(ask_changed)](const std::string& fp) {
    switch (known_hosts->Check(key, fp)) {
      case KnownHosts::Verdict::kTrusted:
        return true;
      case KnownHosts::Verdict::kChanged: {
        const std::string old = known_hosts->Lookup(key).value_or("");
        if (ask_changed && ask_changed(old, fp)) {
          std::string error;
          known_hosts->Remember(key, fp, &error);
          return true;
        }
        if (changed) *changed = true;
        return false;
      }
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
          return raw->SendMessage(type, reinterpret_cast<const uint8_t*>(payload.data()), payload.size()) == RDA_OK;
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
  if (s->clipboard_) s->clipboard_->Start();
  s->thread_ = std::thread([raw = s.get()] { raw->KeepAlive(); });
  return s;
}

rda_status RemoteSession::SendInput(const rda_input_event& ev) {
  std::lock_guard lock(mu_);
  return viewer_ ? rda_viewer_send_input(viewer_, &ev) : RDA_ERR_CLOSED;
}

rda_status RemoteSession::SendMessage(uint32_t type, const uint8_t* payload, size_t n) {
  std::lock_guard lock(mu_);
  return viewer_ ? rda_viewer_send_message(viewer_, type, payload, n) : RDA_ERR_CLOSED;
}

rda_viewer* RemoteSession::viewer() const {
  std::lock_guard lock(mu_);
  return viewer_;
}

SessionReady RemoteSession::session() const {
  std::lock_guard lock(mu_);
  return session_;
}

bool RemoteSession::FollowMove(const SessionReady& ready) {
  rda_viewer* fresh = nullptr;
  std::string error;
  if (CreateSessionViewer(options_.control.host, ready, ViewerConfig(), &fresh, &error) != RDA_OK) {
    if (options_.log) options_.log("moved session: " + error);
    return false;
  }
  const int wait_ms = ready.transport == MediaTransport::kUdp && options_.udp_wait_ms > 0 ? options_.udp_wait_ms
                                                                                         : options_.control.timeout_ms;
  rda_state st = RDA_STATE_CONNECTING;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  while (!stop_ && (st = rda_viewer_get_state(fresh)) == RDA_STATE_CONNECTING &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // Close() may be waiting: check stop_
  }
  if (stop_ || st != RDA_STATE_CONNECTED) {
    rda_viewer_destroy(fresh);
    return false;
  }
  rda_viewer* old = nullptr;
  {
    std::lock_guard lock(mu_);
    old = viewer_;
    viewer_ = fresh;
    session_ = ready;
  }
  if (old) rda_viewer_destroy(old);  // no callback of it runs after this
  if (options_.log) options_.log("session moved: new picture connection");
  if (options_.on_moved) options_.on_moved(ready);
  return true;
}

void RemoteSession::KeepAlive() {
  using Clock = std::chrono::steady_clock;
  // Pings often enough that a server which vanished without a word (power
  // off, network gone) is noticed in seconds: the user sees "interrupted"
  // after kStallAfter and the session ends after kSilenceLimit.
  constexpr auto kPingEvery = std::chrono::seconds(2);
  constexpr auto kStallAfter = std::chrono::seconds(4);
  constexpr auto kSilenceLimit = std::chrono::seconds(12);
  bool stalled = false;
  // The server gives a move 60 seconds (ServerSession::kMoveTimeout).
  constexpr auto kMoveLimit = std::chrono::seconds(75);
  MessageChannel& ch = control_->channel();
  auto last_sent = Clock::now(), last_heard = Clock::now();
  int64_t next_ping = 1;
  bool moving = false;
  bool tcp_asked = false;
  Clock::time_point moving_since{};
  // The picture can drop a moment before the reason arrives on the control
  // connection (bye, session_moving): wait a little before calling it lost.
  constexpr auto kMediaGrace = std::chrono::milliseconds(1500);
  std::optional<Clock::time_point> media_down_since;
  std::string ended;
  while (!stop_ && ended.empty()) {
    auto r = ch.Receive(250);
    const auto now = Clock::now();
    switch (r.status) {
      case MessageChannel::Received::kMessage:
        last_heard = now;
        if (stalled) {
          stalled = false;
          if (options_.on_stalled) options_.on_stalled(false);
        }
        if (auto* ping = std::get_if<Ping>(&*r.message)) {
          ch.Send(Pong{ping->id});
          last_sent = now;
        } else if (auto* bye = std::get_if<Bye>(&*r.message)) {
          ended = bye->reason.empty() ? "server_closed" : bye->reason;
        } else if (auto* err = std::get_if<Error>(&*r.message)) {
          ended = err->code.empty() ? "server_closed" : err->code;
        } else if (auto* mv = std::get_if<SessionMoving>(&*r.message)) {
          if (options_.log) options_.log("session moving: " + mv->reason);
          if (options_.on_moving) options_.on_moving(mv->reason);
          moving = true;
          tcp_asked = false;
          moving_since = now;
        } else if (auto* ready = std::get_if<SessionReady>(&*r.message)) {
          if (FollowMove(*ready)) {
            moving = false;
            media_down_since.reset();
          } else if (ready->transport == MediaTransport::kUdp && !tcp_asked) {
            // UDP did not get through to the new place: ask for TCP there.
            SessionRequest tcp = options_.control.request;
            tcp.transport = MediaTransport::kTcp;
            tcp_asked = ch.Send(tcp);
            moving = true;
            if (!tcp_asked) ended = "connection_lost";
          } else {
            ended = "media_lost";
          }
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
    } else if (!stalled && now - last_heard >= kStallAfter) {
      stalled = true;
      if (options_.on_stalled) options_.on_stalled(true);
    } else if (moving && now - moving_since >= kMoveLimit) {
      ended = "media_lost";
    } else if (!moving && rda_viewer_get_state(viewer()) == RDA_STATE_DISCONNECTED) {
      if (!media_down_since) media_down_since = now;
      if (now - *media_down_since >= kMediaGrace) ended = "media_lost";
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
  rda_viewer* v = nullptr;
  {
    std::lock_guard lock(mu_);
    v = viewer_;
    viewer_ = nullptr;
  }
  if (v) rda_viewer_destroy(v);
  if (control_) {
    control_->Close("user_closed");
    control_.reset();
  }
}

RemoteSession::~RemoteSession() { Close(); }

}  // namespace rdm
