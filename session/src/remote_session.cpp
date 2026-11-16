#include "rdmachine/session/remote_session.h"

#include <algorithm>
#include <chrono>

#include "rdmachine/audio_control.h"
#include "rdmachine/server_session.h"  // ExtraDisplaySecrets
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

// The picture goes to the address the control connection reached, not to a
// new lookup of the host name: a name with several addresses (IPv4 and IPv6,
// or a computer on several networks) could resolve to another one, from
// which the server's packets would not come.
std::string RemoteSession::MediaHostAddress() const {
  const std::string ip = control_ ? control_->peer_ip() : std::string();
  return ip.empty() ? options_.control.host : ip;
}

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
  const auto connect_start = std::chrono::steady_clock::now();
  options.control.request.audio = options.audio;
  *result = ConnectAndStartSession(options.control);
  if (!result->error.empty()) return nullptr;
  const auto connect_took = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - connect_start)
                                .count();

  std::unique_ptr<RemoteSession> s(new RemoteSession());
  s->options_ = std::move(options);
  // How long UDP gets before the picture falls back to TCP: on a far link
  // (a 500 ms round trip) its handshake alone takes about as long as the
  // default, so it grows with what logging in took (a few round trips),
  // up to 15 s.
  if (s->options_.udp_wait_ms > 0) {
    s->udp_wait_ms_ = static_cast<int>(
        std::clamp<long long>(2 * connect_took, s->options_.udp_wait_ms, std::max(s->options_.udp_wait_ms, 15000)));
  }
  s->control_ = std::move(result->connection);
  s->revision_ = result->revision;
  const int connect_ms = s->options_.control.timeout_ms;
  if (s->options_.clipboard && s->options_.clipboard_mode != ClipboardMode::kOff) {
    ClipboardSync::Options so;
    so.mode = s->options_.clipboard_mode;
    so.fetch_on_announce = s->options_.clipboard_fetch_on_announce;
    so.log = s->options_.log;
    so.file_target = s->options_.file_target;
    so.file_staging_dir = s->options_.file_staging_dir;
    RemoteSession* raw = s.get();
    so.send_bulk = [raw](uint32_t type, std::string_view payload) {
      return raw->SendBulkMessage(type, reinterpret_cast<const uint8_t*>(payload.data()), payload.size()) == RDA_OK;
    };
    so.bulk_queued = [raw] { return raw->BulkQueuedBytes(); };
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
    rda_viewer_config vc = s->ViewerConfig();
    vc.audio = s->session_.audio ? 1 : 0;  // the main picture only
    if (CreateSessionViewer(s->MediaHostAddress(), s->session_, vc, &s->viewer_, &error) != RDA_OK) {
      result->error = "media_failed";
      result->detail = error;
      return nullptr;
    }
    const bool can_fall_back = attempt == 0 && s->session_.transport == MediaTransport::kUdp && s->udp_wait_ms_ > 0;
    const rda_state st = WaitConnected(s->viewer_, can_fall_back ? s->udp_wait_ms_ : connect_ms);
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
    if (!AwaitReady(s->control_->channel(), std::max(connect_ms, s->options_.control.session_timeout_ms), result))
      return nullptr;
  }
  result->session = s->session_;
  {
    std::lock_guard lock(s->mu_);
    s->StartPlayer();
  }
  if (s->session_.audio) {
    s->CheckOutputMuted();
    if (s->audio_paused_ || s->output_muted_) s->UpdateAudioPaused(true);
  }
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

rda_status RemoteSession::SendBulkMessage(uint32_t type, const uint8_t* payload, size_t n) {
  std::lock_guard lock(mu_);
  return viewer_ ? rda_viewer_send_bulk_message(viewer_, type, payload, n) : RDA_ERR_CLOSED;
}

int64_t RemoteSession::BulkQueuedBytes() const {
  std::lock_guard lock(mu_);
  return viewer_ ? rda_viewer_bulk_queued_bytes(viewer_) : -1;
}

rda_viewer* RemoteSession::viewer() const {
  std::lock_guard lock(mu_);
  return viewer_;
}

SessionReady RemoteSession::session() const {
  std::lock_guard lock(mu_);
  return session_;
}

void RemoteSession::StartPlayer() {
  if (!session_.audio || !viewer_ || player_) return;
  rda_audio_player_config pc;
  rda_audio_player_config_default(&pc);
  pc.null_device = options_.audio_null_device ? 1 : 0;
  if (rda_audio_player_create(viewer_, &pc, &player_) != RDA_OK) {
    player_ = nullptr;
    if (options_.log) options_.log(std::string("sound: no player: ") + rda_last_error());
    return;
  }
  if (options_.log) options_.log(options_.audio_null_device ? "sound: on (null device)" : "sound: on");
}

void RemoteSession::SendAudioPaused() {
  const std::string payload = EncodeAudioControl({sent_paused_.load()});
  SendMessage(kAudioControlMessageType, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

void RemoteSession::UpdateAudioPaused(bool again) {
  std::lock_guard lock(pause_mu_);
  const bool want = audio_paused_ || output_muted_;
  if (sent_paused_.exchange(want) == want && !again) return;
  SendAudioPaused();
}

void RemoteSession::SetAudioPaused(bool paused) {
  audio_paused_ = paused;
  UpdateAudioPaused(false);
}

void RemoteSession::CheckOutputMuted() {
  int muted = -1;
  if (options_.output_muted) {
    muted = options_.output_muted();
  } else {
    std::lock_guard lock(mu_);
    if (player_) muted = rda_audio_player_output_muted(player_);
  }
  const bool now = muted == 1;  // not known: as audible
  if (output_muted_.exchange(now) == now) return;
  if (options_.log) {
    options_.log(now ? "sound: paused, this Mac's output is muted" : "sound: this Mac's output is audible again");
  }
  UpdateAudioPaused(false);
}

bool RemoteSession::audio_playing() const {
  std::lock_guard lock(mu_);
  return player_ != nullptr;
}

bool RemoteSession::FollowMove(const SessionReady& ready) {
  rda_viewer* fresh = nullptr;
  std::string error;
  rda_viewer_config vc = ViewerConfig();
  vc.audio = ready.audio ? 1 : 0;
  if (CreateSessionViewer(MediaHostAddress(), ready, vc, &fresh, &error) != RDA_OK) {
    if (options_.log) options_.log("moved session: " + error);
    return false;
  }
  const int wait_ms = ready.transport == MediaTransport::kUdp && udp_wait_ms_ > 0 ? udp_wait_ms_
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
  CloseExtras();  // the old desktop's extra displays are gone with it
  rda_viewer* old = nullptr;
  rda_audio_player* old_player = nullptr;
  {
    std::lock_guard lock(mu_);
    old = viewer_;
    old_player = player_;  // it plays the old viewer: goes before it
    player_ = nullptr;
    viewer_ = fresh;
    session_ = ready;
  }
  if (old_player) rda_audio_player_destroy(old_player);
  if (old) rda_viewer_destroy(old);  // no callback of it runs after this
  {
    std::lock_guard lock(mu_);
    StartPlayer();
  }
  // The new place knows nothing of a pause asked for before.
  if (session_.audio && sent_paused_) UpdateAudioPaused(true);
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
  auto next_mute_check = Clock::now();
  while (!stop_ && ended.empty()) {
    auto r = ch.Receive(250);
    const auto now = Clock::now();
    // Nobody hears it while this Mac's output is muted: pause the sound.
    if (now >= next_mute_check) {
      next_mute_check = now + std::chrono::seconds(1);
      if (session().audio) CheckOutputMuted();
    }
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
          const bool followed = FollowMove(*ready);
          // That took seconds during which nothing was read here: the
          // server's messages are waiting, it has not gone silent.
          last_heard = Clock::now();
          if (followed) {
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
    if (last_heard > now) {
      // Just back from following a move: judge the silence on the next round.
    } else if (now - last_heard >= kSilenceLimit) {
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

bool RemoteSession::ConnectExtra(size_t i, rda_viewer_config config, std::string* error) {
  SessionReady ready;
  {
    std::lock_guard lock(mu_);
    if (i >= session_.extra_displays.size()) {
      *error = "no such extra display";
      return false;
    }
    ready = session_;
  }
  const ExtraDisplay& x = ready.extra_displays[i];
  SessionSecrets main;
  main.key = ready.key;
  main.auth_token = ready.auth_token;
  const SessionSecrets own = ExtraDisplaySecrets(main, i);
  ready.port = x.port;
  ready.key = own.key;
  ready.auth_token = own.auth_token;
  ready.desktop = x.desktop;
  rda_viewer* v = nullptr;
  if (CreateSessionViewer(MediaHostAddress(), ready, config, &v, error) != RDA_OK) return false;
  const int wait_ms = ready.transport == MediaTransport::kUdp && udp_wait_ms_ > 0 ? udp_wait_ms_
                                                                                 : options_.control.timeout_ms;
  rda_state st = RDA_STATE_CONNECTING;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  while (!stop_ && (st = rda_viewer_get_state(v)) == RDA_STATE_CONNECTING &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (stop_ || st != RDA_STATE_CONNECTED) {
    rda_viewer_destroy(v);
    *error = "the extra display's picture did not connect";
    return false;
  }
  rda_viewer* old = nullptr;
  {
    std::lock_guard lock(mu_);
    if (extras_.size() <= i) extras_.resize(i + 1, nullptr);
    old = extras_[i];
    extras_[i] = v;
  }
  if (old) rda_viewer_destroy(old);
  if (options_.log) options_.log("extra display " + std::to_string(i + 2) + " connected");
  return true;
}

rda_status RemoteSession::SendExtraInput(size_t i, const rda_input_event& ev) {
  std::lock_guard lock(mu_);
  return i < extras_.size() && extras_[i] ? rda_viewer_send_input(extras_[i], &ev) : RDA_ERR_CLOSED;
}

void RemoteSession::CloseExtras() {
  std::vector<rda_viewer*> extras;
  {
    std::lock_guard lock(mu_);
    extras.swap(extras_);
  }
  for (rda_viewer* v : extras) {
    if (v) rda_viewer_destroy(v);  // no callback of it runs after this
  }
}

void RemoteSession::Close() {
  if (clipboard_) clipboard_->Stop();  // before the viewer it sends through
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  CloseExtras();
  rda_viewer* v = nullptr;
  rda_audio_player* p = nullptr;
  {
    std::lock_guard lock(mu_);
    v = viewer_;
    viewer_ = nullptr;
    p = player_;
    player_ = nullptr;
  }
  if (p) rda_audio_player_destroy(p);  // before its viewer
  if (v) rda_viewer_destroy(v);
  if (control_) {
    control_->Close("user_closed");
    control_.reset();
  }
}

RemoteSession::~RemoteSession() { Close(); }

}  // namespace rdm
