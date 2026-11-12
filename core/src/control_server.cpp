#include "rdmachine/control_server.h"

#include <algorithm>
#include <vector>

#include <chrono>
#include <cstdio>

#include "rdmachine/channel.h"

namespace rdm {

namespace {
constexpr int kAcceptPollMs = 200;
}

ControlServer::ControlServer(ServerDelegate& delegate, TlsIdentity identity, ControlServerOptions options)
    : delegate_(delegate), identity_(std::move(identity)), options_(std::move(options)) {}

ControlServer::~ControlServer() { Stop(); }

bool ControlServer::Start(std::string* error) {
  if (listener_) return true;
  listener_ = TlsListener::Listen(options_.bind_address, options_.port, identity_, error);
  if (!listener_) return false;
  stop_ = false;
  accept_thread_ = std::thread([this] { AcceptLoop(); });
  return true;
}

void ControlServer::Stop() {
  if (!listener_) return;
  stop_ = true;
  if (accept_thread_.joinable()) accept_thread_.join();
  listener_->Close();
  // The connection loops notice stop_ within a tick and send bye
  // "server_shutdown" (the client then knows to connect again); give them
  // that moment before cutting what is left.
  for (int i = 0; i < 20 && active_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  {
    std::lock_guard lock(conns_mu_);
    for (const Open& o : conns_) o.conn->Shutdown();
  }
  ReapWorkers(true);
  listener_.reset();
}

Clock::duration ControlServer::PairingCodeRemaining() {
  return options_.offer_pairing_codes ? pairing_.remaining(Clock::now()) : Clock::duration::zero();
}

void ControlServer::CloseConnections() {
  std::lock_guard lock(conns_mu_);
  for (const Open& o : conns_) o.disconnected->store(true);  // each loop says bye and closes
}

uint16_t ControlServer::port() const { return listener_ ? listener_->port() : 0; }

std::string ControlServer::IssuePairingCode() {
  if (!options_.offer_pairing_codes) return {};
  return pairing_.Issue(Clock::now());
}

void ControlServer::ReapWorkers(bool all) {
  std::list<Worker> finished;
  {
    std::lock_guard lock(threads_mu_);
    for (auto it = threads_.begin(); it != threads_.end();) {
      if (all || it->done->load()) {
        finished.splice(finished.end(), threads_, it++);
      } else {
        ++it;
      }
    }
  }
  for (Worker& w : finished) w.thread.join();
}

void ControlServer::AcceptLoop() {
  while (!stop_) {
    std::string error;
    std::unique_ptr<TlsConnection> conn = listener_->Accept(kAcceptPollMs, &error);
    ReapWorkers(false);
    if (!conn) {
      if (!error.empty()) std::fprintf(stderr, "rdmachine: %s\n", error.c_str());
      continue;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard lock(threads_mu_);
    threads_.push_back(Worker{std::thread([this, c = std::move(conn), done]() mutable {
                                Serve(std::move(c));
                                done->store(true);
                              }),
                              done});
  }
}

bool ControlServer::TakeOver(TlsConnection* self) {
  std::unique_lock lock(conns_mu_);
  std::vector<TlsConnection*> others;
  for (const Open& o : conns_) {
    if (o.conn == self) continue;
    o.replaced->store(true);
    others.push_back(o.conn);
  }
  if (others.empty()) return false;
  return conns_cv_.wait_for(lock, std::chrono::seconds(5), [&] {
    for (const Open& o : conns_) {
      if (std::find(others.begin(), others.end(), o.conn) != others.end()) return false;
    }
    return true;
  });
}

void ControlServer::Serve(std::unique_ptr<TlsConnection> conn) {
  ++active_;
  auto replaced = std::make_shared<std::atomic<bool>>(false);
  auto disconnected = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard lock(conns_mu_);
    conns_.push_back(Open{conn.get(), replaced, disconnected});
  }
  // Stop() may have shut everything down just before this connection was
  // registered; check again so it does not linger.
  if (!stop_) {
    std::string error;
    if (!conn->ServerHandshake(options_.handshake_timeout_ms, &error)) {
      std::fprintf(stderr, "rdmachine: %s from %s\n", error.c_str(), conn->peer_address().c_str());
    } else {
      MessageChannel channel(*conn);
      ServerGuards guards;
      guards.limiter = &limiter_;
      guards.pairing = options_.offer_pairing_codes ? &pairing_ : nullptr;
      TlsConnection* self = conn.get();
      guards.take_over = [this, self] { return TakeOver(self); };
      ServerSession session(delegate_, guards, conn->peer_ip());
      auto last_heard = Clock::now(), last_ping = Clock::now();
      int64_t next_ping = 1;
      bool open = true;
      // Short waits, so that a session the delegate wants moved or ended
      // (ServerSession::Tick) is acted on promptly.
      constexpr int kTickMs = 250;
      while (open && !stop_) {
        auto r = channel.Receive(kTickMs);
        ServerSession::Output out;
        const auto now = Clock::now();
        switch (r.status) {
          case MessageChannel::Received::kMessage:
            last_heard = now;
            out = session.Handle(*r.message);
            break;
          case MessageChannel::Received::kProtocolError:
            out = session.HandleBadMessage(r.error);
            break;
          case MessageChannel::Received::kClosed:
            open = false;
            continue;
          case MessageChannel::Received::kTimeout:
            if (now - last_heard >= std::chrono::milliseconds(options_.idle_timeout_ms)) {
              out.send.push_back(Bye{"idle_timeout"});
              out.close = true;
            } else if (now - last_heard >= std::chrono::milliseconds(options_.ping_interval_ms) &&
                       now - last_ping >= std::chrono::milliseconds(options_.ping_interval_ms)) {
              out.send.push_back(Ping{next_ping++});
              last_ping = now;
            }
            break;
        }
        if (!out.close && replaced->load()) {
          // A newer connection takes over (TakeOver waits for this close).
          out.send.push_back(Bye{"session_replaced"});
          out.close = true;
        } else if (!out.close && disconnected->load()) {
          out.send.push_back(Bye{"server_disconnected"});
          out.close = true;
        }
        if (!out.close) {
          ServerSession::Output tick = session.Tick();
          for (Message& m : tick.send) out.send.push_back(std::move(m));
          out.close = out.close || tick.close;
        }
        for (const Message& m : out.send) {
          if (!channel.Send(m)) {
            open = false;
            break;
          }
        }
        if (out.close) open = false;
      }
      if (stop_ && session.state() != ServerSession::State::kClosed) channel.Send(Bye{"server_shutdown"});
    }
  }
  {
    std::lock_guard lock(conns_mu_);
    conns_.remove_if([&](const Open& o) { return o.conn == conn.get(); });
  }
  conns_cv_.notify_all();
  conn->Close();
  --active_;
}

}  // namespace rdm
