#include "rdmachine/session/agent_host.h"

#include <unistd.h>

namespace rdm {

using json = nlohmann::json;

AgentHost::AgentHost(MediaHost& media, AgentHostOptions options) : media_(media), options_(std::move(options)) {}

AgentHost::~AgentHost() { Stop(); }

void AgentHost::Log(const std::string& line) {
  if (options_.log) options_.log(line);
}

void AgentHost::Start() {
  stop_ = false;
  thread_ = std::thread(&AgentHost::Run, this);
}

void AgentHost::Stop() {
  if (stop_.exchange(true)) return;
  {
    std::lock_guard lock(mu_);
    if (channel_) {
      // Quitting on purpose: the daemon ends the session at once instead of
      // waiting for this agent to come back.
      channel_->Send({{"type", "leaving"}});
      channel_->Shutdown();
    }
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

std::string AgentHost::fingerprint() const {
  std::lock_guard lock(mu_);
  return fingerprint_;
}

uint16_t AgentHost::port() const {
  std::lock_guard lock(mu_);
  return port_;
}

std::string AgentHost::server_name() const {
  std::lock_guard lock(mu_);
  return server_name_;
}

void AgentHost::Run() {
  bool said_waiting = false;
  while (!stop_) {
    std::string error;
    const int fd = UnixConnect(options_.socket_path, &error);
    if (fd < 0) {
      if (!said_waiting) Log("waiting for the rdmachine daemon (" + error + ")");
      said_waiting = true;
      for (int i = 0; i < 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    said_waiting = false;
    AgentChannel channel(fd);
    {
      std::lock_guard lock(mu_);
      channel_ = &channel;
    }
    if (channel.Send({{"type", "hello"}, {"kind", options_.kind}, {"version", kAgentLinkVersion},
                      {"pid", static_cast<int64_t>(getpid())}, {"server_version", VersionString()}})) {
      Serve(channel);
    }
    {
      std::lock_guard lock(mu_);
      channel_ = nullptr;
      fingerprint_.clear();
      port_ = 0;
    }
    connected_ = false;
    cv_.notify_all();
    // Whatever ran belonged to that daemon's connection.
    bool was_in_session;
    {
      std::lock_guard lock(mu_);
      was_in_session = in_session_;
      in_session_ = false;
    }
    if (was_in_session) media_.EndSession();
    if (!stop_) {
      Log("lost the rdmachine daemon; connecting again");
      for (int i = 0; i < 10 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void AgentHost::Serve(AgentChannel& channel) {
  while (!stop_) {
    auto r = channel.Receive(500);
    if (r.status == AgentChannel::Received::kTimeout) continue;
    if (r.status != AgentChannel::Received::kMessage) return;
    Handle(channel, r.message);
  }
}

void AgentHost::Handle(AgentChannel& channel, const json& m) {
  const std::string type = m.value("type", "");
  const int64_t id = m.value("id", static_cast<int64_t>(0));
  if (type == "welcome") {
    {
      std::lock_guard lock(mu_);
      fingerprint_ = m.value("fingerprint", "");
      server_name_ = m.value("server_name", "");
      port_ = static_cast<uint16_t>(m.value("port", 0));
    }
    connected_ = true;
    Log("connected to the rdmachine daemon as the " + options_.kind + " agent");
    if (options_.send_settings && options_.port > 0) {
      channel.Send({{"type", "settings"}, {"port", options_.port}, {"offer_pairing_codes", options_.offer_pairing_codes}});
    }
    return;
  }
  if (type == "start_session") {
    auto request = SessionRequestFromAgentJson(m.value("request", json::object()));
    auto secrets = SessionSecretsFromAgentJson(m.value("secrets", json::object()));
    const SessionOrigin origin = SessionOriginFromAgentJson(m.value("origin", json::object()));
    json reply = {{"reply_to", id}};
    if (!request || !secrets) {
      reply["type"] = "session_failed";
      reply["error"] = "internal";
      channel.Send(reply);
      return;
    }
    if (m.contains("login") && m["login"].is_object()) {
      std::string user = m["login"].value("username", "");
      std::string pass = m["login"].value("password", "");
      media_.ExpectLogin(user, pass);
      volatile char* p = pass.data();
      for (size_t i = 0; i < pass.size(); ++i) p[i] = 0;
    }
    std::string error;
    auto grant = media_.StartSession(*request, *secrets, origin, &error);
    std::fill(secrets->key.begin(), secrets->key.end(), 0);
    if (grant) {
      {
        std::lock_guard lock(mu_);
        in_session_ = true;
      }
      reply["type"] = "session_started";
      reply["grant"] = ToAgentJson(*grant);
    } else {
      reply["type"] = "session_failed";
      reply["error"] = error.empty() ? "internal" : error;
    }
    channel.Send(reply);
    return;
  }
  if (type == "end_session") {
    bool running;
    {
      std::lock_guard lock(mu_);
      running = in_session_;
      in_session_ = false;
    }
    if (running) media_.EndSession();
    channel.Send({{"type", "session_ended"}, {"reply_to", id}});
    return;
  }
  // Answers to Ask().
  std::lock_guard lock(mu_);
  if (!reply_type_.empty() && type == reply_type_ && id == reply_id_) {
    reply_ = m;
    cv_.notify_all();
  }
}

std::optional<json> AgentHost::Ask(json m, const std::string& reply_type, int timeout_ms) {
  std::unique_lock lock(mu_);
  if (!channel_ || !connected_) return std::nullopt;
  const int64_t id = next_id_++;
  m["id"] = id;
  reply_.reset();
  reply_type_ = reply_type;
  reply_id_ = id;
  AgentChannel* ch = channel_;
  lock.unlock();
  if (!ch->Send(m)) return std::nullopt;
  lock.lock();
  cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return reply_.has_value() || stop_ || !channel_; });
  reply_type_.clear();
  auto out = std::move(reply_);
  reply_.reset();
  return out;
}

AgentHost::PairingCode AgentHost::IssuePairingCode() {
  PairingCode out;
  if (auto r = Ask({{"type", "issue_pairing_code"}}, "pairing_code", 3000)) {
    out.code = r->value("code", "");
    out.remaining = std::chrono::milliseconds(r->value("remaining_ms", static_cast<int64_t>(0)));
  }
  return out;
}

std::chrono::milliseconds AgentHost::PairingCodeRemaining() {
  if (auto r = Ask({{"type", "pairing_code_status"}}, "pairing_code", 1000)) {
    return std::chrono::milliseconds(r->value("remaining_ms", static_cast<int64_t>(0)));
  }
  return std::chrono::milliseconds(0);
}

void AgentHost::Disconnect() {
  std::lock_guard lock(mu_);
  if (channel_) channel_->Send({{"type", "disconnect"}});
}

void AgentHost::SendSettings(uint16_t port, bool offer_pairing_codes) {
  std::lock_guard lock(mu_);
  if (channel_) channel_->Send({{"type", "settings"}, {"port", port}, {"offer_pairing_codes", offer_pairing_codes}});
}

}  // namespace rdm
