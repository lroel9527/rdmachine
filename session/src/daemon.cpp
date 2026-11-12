#include "rdmachine/session/daemon.h"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "rdmachine/session/system_auth.h"

namespace rdm {

using json = nlohmann::json;

namespace {

constexpr auto kPendingLoginLifetime = std::chrono::seconds(60);

void Wipe(std::string& s) {
  volatile char* p = s.data();
  for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
  s.clear();
}

std::string UserName(uint32_t uid) {
  if (passwd* pw = getpwuid(uid)) return pw->pw_name;
  return std::to_string(uid);
}

}  // namespace

ConsoleOwner CurrentConsoleOwner() {
  ConsoleOwner c;
  struct stat st {};
  if (stat("/dev/console", &st) == 0) c.uid = st.st_uid;
  c.user = c.uid == 0 ? std::string() : UserName(c.uid);
  return c;
}

Daemon::Daemon(DaemonOptions options) : options_(std::move(options)) {}

Daemon::~Daemon() { Stop(); }

void Daemon::Log(const std::string& line) {
  if (options_.log) options_.log(line);
}

ConsoleOwner Daemon::Console() { return options_.console ? options_.console() : CurrentConsoleOwner(); }

bool Daemon::Start(std::string* error) {
  const std::string dir = options_.socket_path.substr(0, options_.socket_path.rfind('/'));
  if (!dir.empty()) {
    mkdir(dir.c_str(), 0755);
    chmod(dir.c_str(), 0755);
  }
  // Anyone may connect; who is trusted is decided per connection (code
  // signature, and which session owns the console).
  listen_fd_ = UnixListen(options_.socket_path, 0666, error);
  if (listen_fd_ < 0) return false;
  stop_ = false;
  accept_thread_ = std::thread(&Daemon::AcceptLoop, this);
  Log("waiting for agents on " + options_.socket_path);
  return true;
}

void Daemon::Stop() {
  if (stop_.exchange(true)) return;
  if (accept_thread_.joinable()) accept_thread_.join();
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    unlink(options_.socket_path.c_str());
    listen_fd_ = -1;
  }
  std::map<int64_t, std::shared_ptr<Agent>> agents;
  {
    std::lock_guard lock(mu_);
    agents.swap(agents_);
    session_agent_.reset();
    Wipe(pending_login_.password);
  }
  cv_.notify_all();
  for (auto& [id, a] : agents) a->channel->Shutdown();
  for (auto& [id, a] : agents) {
    if (a->reader.joinable()) a->reader.join();
  }
}

void Daemon::AcceptLoop() {
  while (!stop_) {
    std::string error;
    const int fd = UnixAccept(listen_fd_, 250, &error);
    if (fd < 0) {
      if (!error.empty()) Log("agent socket: " + error);
      continue;
    }
    std::string why;
    const bool trusted = options_.trust_agent ? options_.trust_agent(fd, &why) : PeerSignedLikeUs(fd, &why);
    if (!trusted) {
      Log("agent refused: " + why);
      close(fd);
      continue;
    }
    if (!why.empty()) Log(why);
    auto agent = std::make_shared<Agent>();
    agent->channel = std::make_unique<AgentChannel>(fd);
    if (auto peer = PeerOf(fd)) agent->uid = peer->uid;
    {
      std::lock_guard lock(mu_);
      agent->id = next_agent_id_++;
      agents_[agent->id] = agent;
    }
    agent->reader = std::thread(&Daemon::ReadLoop, this, agent);
  }
}

void Daemon::ReadLoop(std::shared_ptr<Agent> agent) {
  while (!stop_) {
    auto r = agent->channel->Receive(500);
    if (r.status == AgentChannel::Received::kTimeout) continue;
    if (r.status != AgentChannel::Received::kMessage) break;
    Handle(agent, r.message);
  }
  {
    std::lock_guard lock(mu_);
    agent->gone_at = std::chrono::steady_clock::now();
  }
  agent->gone = true;
  if (!agent->kind.empty()) Log("agent left: " + agent->kind + (agent->user.empty() ? "" : " of " + agent->user));
  std::lock_guard lock(mu_);
  cv_.notify_all();
  // Stop() joins the readers itself; otherwise this thread forgets the agent
  // and detaches itself. Nothing of `this` is used after that.
  auto it = agents_.find(agent->id);
  if (it != agents_.end() && !stop_) {
    it->second->reader.detach();
    agents_.erase(it);
  }
}

void Daemon::Handle(const std::shared_ptr<Agent>& agent, const json& m) {
  const std::string type = m.value("type", "");
  if (type == "hello") {
    const std::string kind = m.value("kind", "");
    if ((kind != "aqua" && kind != "loginwindow") || m.value("version", 0) != kAgentLinkVersion) {
      Log("agent refused: kind \"" + kind + "\", link version " + std::to_string(m.value("version", 0)));
      agent->channel->Shutdown();
      return;
    }
    // The login window runs as root; a user's session as that user.
    if (options_.check_agent_uid && (kind == "loginwindow") != (agent->uid == 0)) {
      Log("agent refused: a " + kind + " agent running as uid " + std::to_string(agent->uid));
      agent->channel->Shutdown();
      return;
    }
    {
      std::lock_guard lock(mu_);
      agent->kind = kind;
      agent->user = kind == "loginwindow" ? std::string() : UserName(agent->uid);
    }
    Log("agent joined: " + kind + (agent->user.empty() ? "" : " of " + agent->user));
    json w = {{"type", "welcome"}, {"version", kAgentLinkVersion}, {"server_name", options_.server_name}};
    if (options_.fingerprint) w["fingerprint"] = options_.fingerprint();
    if (options_.port) w["port"] = options_.port();
    agent->channel->Send(w);
    cv_.notify_all();
    return;
  }
  if (agent->kind.empty()) return;  // nothing before hello
  if (m.contains("reply_to") && m["reply_to"].is_number_integer()) {
    {
      std::lock_guard lock(mu_);
      replies_[m["reply_to"].get<int64_t>()] = m;
    }
    cv_.notify_all();
    return;
  }
  // Requests from the menu bar of the console user's session only.
  const ConsoleOwner console = Console();
  const bool console_agent = agent->kind == "aqua" && agent->uid == console.uid;
  if (type == "issue_pairing_code") {
    json reply = {{"type", "pairing_code"}, {"id", m.value("id", 0)}};
    if (console_agent && options_.issue_pairing_code) {
      reply["code"] = options_.issue_pairing_code();
      if (options_.pairing_code_remaining_ms) reply["remaining_ms"] = options_.pairing_code_remaining_ms();
    } else {
      reply["code"] = "";
    }
    agent->channel->Send(reply);
  } else if (type == "pairing_code_status") {
    json reply = {{"type", "pairing_code"}, {"id", m.value("id", 0)}, {"code", ""}};
    reply["remaining_ms"] = console_agent && options_.pairing_code_remaining_ms ? options_.pairing_code_remaining_ms() : 0;
    agent->channel->Send(reply);
  } else if (type == "disconnect") {
    if (console_agent && options_.close_connections) {
      Log("disconnected by the user at this computer");
      options_.close_connections();
    }
  } else if (type == "settings") {
    if (console_agent && options_.settings_changed && m.contains("port") && m["port"].is_number_integer()) {
      const int64_t port = m["port"].get<int64_t>();
      if (port > 0 && port < 65535) options_.settings_changed(static_cast<uint16_t>(port), m.value("offer_pairing_codes", true));
    }
  } else if (type == "leaving") {
    agent->leaving = true;  // quit or stopped on purpose; see CheckSession
  } else if (type == "ended") {
    // The agent's session ended by itself (the viewer left); the control
    // connection notices through its own checks.
  }
}

std::shared_ptr<Daemon::Agent> Daemon::ActiveAgentLocked(const ConsoleOwner& console) {
  std::shared_ptr<Agent> best;
  for (auto& [id, a] : agents_) {
    if (a->gone || a->kind.empty()) continue;
    const bool fits = console.login_window() ? a->kind == "loginwindow" : (a->kind == "aqua" && a->uid == console.uid);
    if (fits && (!best || a->id > best->id)) best = a;
  }
  return best;
}

std::optional<json> Daemon::Request(const std::shared_ptr<Agent>& agent, json m, int timeout_ms) {
  int64_t id;
  {
    std::lock_guard lock(mu_);
    id = next_request_id_++;
  }
  m["id"] = id;
  if (!agent->channel->Send(m)) return std::nullopt;
  std::unique_lock lock(mu_);
  const bool got = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
    return stop_ || agent->gone || replies_.count(id) > 0;
  });
  if (!got || !replies_.count(id)) {
    replies_.erase(id);
    return std::nullopt;
  }
  json reply = std::move(replies_[id]);
  replies_.erase(id);
  return reply;
}

std::string Daemon::ServerName() { return options_.server_name; }

bool Daemon::CheckPassword(const std::string& username, const std::string& password, std::string* error) {
  const ConsoleOwner console = Console();
  // Someone is logged in: only that user (the desktop is theirs). At the
  // login window: any account of this computer.
  if (!console.login_window() && username != console.user) {
    *error = "not_console_user";
    Log("login refused: " + username + " is not the logged-in user (" + console.user + ")");
    return false;
  }
  bool ok = false;
  if (options_.check_password) {
    ok = options_.check_password(username, password, error);
  } else {
    std::string detail;
    const std::string service = options_.pam_service.empty() ? DefaultPamService() : options_.pam_service;
    ok = CheckSystemPassword(service, username, password, error, &detail);
    if (!ok) Log("password login for " + username + ": " + *error + " (" + detail + ")");
  }
  if (ok) {
    Log("password login for " + username + ": ok" + (console.login_window() ? " (at the login window)" : ""));
    std::lock_guard lock(mu_);
    Wipe(pending_login_.password);
    pending_login_ = PendingLogin{username, password, std::chrono::steady_clock::now()};
  }
  return ok;
}

std::optional<SessionGrant> Daemon::StartSession(const SessionRequest& request, const SessionSecrets& secrets,
                                                 const SessionOrigin& origin, std::string* error) {
  const ConsoleOwner console = Console();
  std::shared_ptr<Agent> agent;
  PendingLogin login;
  {
    std::lock_guard lock(mu_);
    if (session_agent_) {
      *error = "busy";
      return std::nullopt;
    }
    agent = ActiveAgentLocked(console);
    // The password goes along only on the first start after the login: a
    // session that moves to the login window (the user logged out) must not
    // log them straight back in.
    if (origin.method == AuthMethod::kPassword && pending_login_.username == origin.username &&
        !pending_login_.password.empty() &&
        std::chrono::steady_clock::now() - pending_login_.at < kPendingLoginLifetime) {
      login = pending_login_;
    }
    Wipe(pending_login_.password);
    pending_login_ = PendingLogin{};
  }
  if (!agent) {
    Wipe(login.password);
    *error = "unavailable";
    Log(std::string("no agent for ") + (console.login_window() ? "the login window" : console.user + "'s session") +
        " yet");
    return std::nullopt;
  }
  json m = {{"type", "start_session"},
            {"request", ToAgentJson(request)},
            {"secrets", ToAgentJson(secrets)},
            {"origin", ToAgentJson(origin)}};
  if (!login.password.empty()) m["login"] = {{"username", login.username}, {"password", login.password}};
  Wipe(login.password);
  auto reply = Request(agent, std::move(m), options_.reply_timeout_ms);
  if (!reply) {
    *error = "unavailable";
    Log("the agent did not answer start_session");
    return std::nullopt;
  }
  if (reply->value("type", "") != "session_started") {
    *error = reply->value("error", "internal");
    return std::nullopt;
  }
  auto grant = SessionGrantFromAgentJson(reply->value("grant", json::object()));
  if (!grant) {
    *error = "internal";
    return std::nullopt;
  }
  {
    std::lock_guard lock(mu_);
    session_agent_ = agent;
    stopped_.active = false;
  }
  Log("session started in " + (agent->kind == "loginwindow" ? std::string("the login window") : agent->user + "'s session"));
  return grant;
}

void Daemon::EndSession() {
  std::shared_ptr<Agent> agent;
  {
    std::lock_guard lock(mu_);
    agent.swap(session_agent_);
  }
  if (!agent || agent->gone) return;
  // Waits for the agent to let go of the media ports, so a session started
  // right after (a move, or TCP after UDP) can bind them.
  if (!Request(agent, {{"type", "end_session"}}, 5000)) Log("the agent did not confirm the end of the session");
}

SessionCheck Daemon::CheckSession(const SessionOrigin& origin) {
  const ConsoleOwner console = Console();
  std::lock_guard lock(mu_);
  // Whoever owns the console now must be the account the login was for, or
  // no one (the login window shows nothing of anyone's).
  const bool allowed = console.login_window() || (!origin.account.empty() && origin.account == console.user);
  if (!allowed) return {SessionCheck::Action::kEnd, "console_changed"};
  if (!session_agent_) {
    // Moving. If the server of the session was stopped on purpose and the
    // console still has the same owner, give it a few seconds to come back
    // (rdmachine-service restart), then end with "server_shutdown" so the
    // client knows (Quit in the menu stays quit).
    if (stopped_.active) {
      const bool same_owner = stopped_.login_window ? console.login_window() : console.uid == stopped_.uid;
      if (same_owner && !ActiveAgentLocked(console) &&
          std::chrono::steady_clock::now() - stopped_.at >= std::chrono::seconds(5)) {
        stopped_.active = false;
        return {SessionCheck::Action::kEnd, "server_shutdown"};
      }
    }
    return {};  // StartSession finds the new agent
  }
  const std::shared_ptr<Agent> active = ActiveAgentLocked(console);
  if (!session_agent_->gone && active == session_agent_) return {};
  if (!session_agent_->gone && !active) {
    // The new session's agent is not there yet (it starts with the login):
    // move now anyway, the restart waits for it.
  }
  const bool same_owner = session_agent_->kind == "loginwindow" ? console.login_window()
                                                                : console.uid == session_agent_->uid;
  if (session_agent_->gone && session_agent_->leaving && same_owner) {
    // Quit or stopped on purpose: tell the client now (so it does not take
    // the dropped picture for a lost connection), then see above.
    stopped_ = Stopped{true, session_agent_->kind == "loginwindow", session_agent_->uid, session_agent_->gone_at};
    return {SessionCheck::Action::kMove, "server_stopping"};
  }
  std::string reason = console.login_window() ? "user_logged_out"
                       : session_agent_->kind == "loginwindow" ? "user_logged_in"
                       : session_agent_->gone && session_agent_->uid == console.uid ? "agent_restarted"
                                                                                    : "user_switched";
  return {SessionCheck::Action::kMove, reason};
}

std::string Daemon::PairingAccount() {
  // The code was shown on the console user's menu bar: their desktop.
  return Console().user;
}

int Daemon::agent_count() {
  std::lock_guard lock(mu_);
  int n = 0;
  for (auto& [id, a] : agents_) n += (!a->gone && !a->kind.empty()) ? 1 : 0;
  return n;
}

bool Daemon::has_active_agent() {
  const ConsoleOwner console = Console();
  std::lock_guard lock(mu_);
  return ActiveAgentLocked(console) != nullptr;
}

}  // namespace rdm
