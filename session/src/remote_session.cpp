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

std::unique_ptr<RemoteSession> RemoteSession::Start(Options options, ConnectResult* result) {
  *result = ConnectAndStartSession(options.control);
  if (!result->error.empty()) return nullptr;

  std::unique_ptr<RemoteSession> s(new RemoteSession());
  s->options_ = std::move(options);
  s->control_ = std::move(result->connection);
  const int connect_ms = s->options_.control.timeout_ms;

  for (int attempt = 0;; ++attempt) {
    s->session_ = result->session;
    std::string error;
    if (CreateSessionViewer(s->options_.control.host, s->session_, s->options_.viewer, &s->viewer_, &error) != RDA_OK) {
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
