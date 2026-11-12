#include "server_runtime.h"

#include "rdmachine/session/system_clipboard.h"

namespace rdm {

ServerSettings SettingsOverrides::Apply(ServerSettings s) const {
  if (port) s.port = *port;
  if (offer_pairing_codes) s.offer_pairing_codes = *offer_pairing_codes;
  if (view_only) s.view_only = *view_only;
  if (clipboard_mode) s.clipboard_mode = *clipboard_mode;
  if (fps) s.fps = *fps;
  if (bitrate_kbps) s.bitrate_kbps = *bitrate_kbps;
  return s;
}

std::string SettingsOverrides::Describe() const {
  std::string out;
  auto add = [&](bool set, const char* name) {
    if (!set) return;
    if (!out.empty()) out += ", ";
    out += name;
  };
  add(port.has_value(), "--port");
  add(offer_pairing_codes.has_value(), "--no-pairing");
  add(view_only.has_value(), "--view-only");
  add(clipboard_mode.has_value(), "--clipboard");
  add(fps.has_value(), "--fps");
  add(bitrate_kbps.has_value(), "--bitrate-kbps");
  return out;
}

ServerRuntime::ServerRuntime(Options options, TlsIdentity identity)
    : options_(std::move(options)), identity_(std::move(identity)) {}

ServerRuntime::~ServerRuntime() { Stop(); }

void ServerRuntime::Log(const std::string& line) const {
  if (options_.log) options_.log(line);
}

bool ServerRuntime::Start(std::string* error) {
  ServerSettings saved;
  if (!LoadServerSettings(options_.config_dir, &saved, error)) return false;
  std::lock_guard lock(mu_);
  return StartLocked(saved, error);
}

bool ServerRuntime::ApplySettings(const ServerSettings& saved, std::string* error) {
  if (!SaveServerSettings(options_.config_dir, saved, error)) return false;
  std::lock_guard lock(mu_);
  const ServerSettings before = saved_;
  StopLocked();
  if (StartLocked(saved, error)) return true;
  // E.g. the new port is taken: keep serving on the old settings, but the
  // file already has the new ones; put the old ones back.
  std::string ignored;
  SaveServerSettings(options_.config_dir, before, &ignored);
  StartLocked(before, &ignored);
  return false;
}

bool ServerRuntime::StartLocked(const ServerSettings& saved, std::string* error) {
  const ServerSettings s = options_.overrides.Apply(saved);
  MediaHostOptions mo;
  mo.server_name = options_.server_name;
  mo.udp_port = s.port;
  mo.tcp_port = static_cast<uint16_t>(s.port + 1);
  mo.pam_service = options_.pam_service;
  mo.login_window = options_.login_window;
  mo.inject_input = !s.view_only;
  // No one's clipboard at the login window.
  mo.clipboard = options_.login_window ? nullptr : SystemClipboard();
  mo.clipboard_mode = s.clipboard_mode;
  mo.virtual_desktop = SystemVirtualDesktop();
  mo.fps = s.fps;
  mo.bitrate_bps = static_cast<int64_t>(s.bitrate_kbps) * 1000;
  mo.log = options_.log;
  auto media = std::make_unique<MediaHost>(mo);

  if (agent_mode()) {
    AgentHostOptions ao;
    ao.socket_path = options_.agent_socket;
    ao.kind = options_.agent_kind;
    ao.log = options_.log;
    ao.send_settings = options_.agent_kind == "aqua";
    ao.port = s.port;
    ao.offer_pairing_codes = s.offer_pairing_codes;
    auto agent = std::make_unique<AgentHost>(*media, ao);
    agent->Start();
    media_ = std::move(media);
    agent_ = std::move(agent);
    saved_ = saved;
    code_.clear();
    Log(std::string("running as the ") + options_.agent_kind + " agent of the rdmachine daemon" +
        (s.view_only ? " (view only)" : "") + ", clipboard " + ClipboardModeName(mo.clipboard ? s.clipboard_mode : ClipboardMode::kOff));
    return true;
  }

  ControlServerOptions co;
  co.bind_address = options_.bind_address;
  co.port = s.port;
  co.offer_pairing_codes = s.offer_pairing_codes && !options_.login_window;
  auto server = std::make_unique<ControlServer>(*media, identity_, co);
  if (!server->Start(error)) return false;
  media_ = std::move(media);
  server_ = std::move(server);
  saved_ = saved;
  code_.clear();
  Log("listening on port " + std::to_string(server_->port()) + (s.view_only ? " (view only)" : "") +
      ", clipboard " + ClipboardModeName(s.clipboard_mode) +
      (s.offer_pairing_codes && !options_.login_window ? "" : ", no pairing codes") +
      (options_.login_window ? ", login window" : ""));
  return true;
}

void ServerRuntime::Stop() {
  std::lock_guard lock(mu_);
  StopLocked();
}

void ServerRuntime::StopLocked() {
  if (server_) server_->Stop();  // ends the session through the delegate
  server_.reset();
  agent_.reset();  // ends a session it runs
  media_.reset();  // after the server that refers to it
  code_.clear();
}

ServerSettings ServerRuntime::saved_settings() const {
  std::lock_guard lock(mu_);
  return saved_;
}

ServerSettings ServerRuntime::effective_settings() const {
  std::lock_guard lock(mu_);
  return options_.overrides.Apply(saved_);
}

bool ServerRuntime::running() const {
  std::lock_guard lock(mu_);
  return server_ != nullptr || agent_ != nullptr;
}

uint16_t ServerRuntime::port() const {
  std::lock_guard lock(mu_);
  if (agent_) return agent_->port();
  return server_ ? server_->port() : 0;
}

bool ServerRuntime::daemon_connected() const {
  std::lock_guard lock(mu_);
  return agent_ && agent_->connected();
}

std::string ServerRuntime::daemon_fingerprint() const {
  std::lock_guard lock(mu_);
  return agent_ ? agent_->fingerprint() : std::string();
}

std::string ServerRuntime::IssuePairingCode() {
  std::lock_guard lock(mu_);
  if (agent_) {
    const AgentHost::PairingCode c = agent_->IssuePairingCode();
    code_ = c.code;
    code_expires_ = Clock::now() + c.remaining;
    return code_;
  }
  if (!server_) return {};
  code_ = server_->IssuePairingCode();
  return code_;
}

ServerRuntime::PairingCode ServerRuntime::CurrentPairingCode() const {
  std::lock_guard lock(mu_);
  PairingCode out;
  if (agent_) {
    // The daemon's code; it may have been used meanwhile (shown until it lapses).
    const auto left = code_expires_ - Clock::now();
    if (code_.empty() || left <= Clock::duration::zero()) return out;
    out.code = code_;
    out.remaining = std::chrono::ceil<std::chrono::seconds>(left);
    return out;
  }
  if (!server_ || code_.empty()) return out;
  const auto left = server_->PairingCodeRemaining();
  if (left <= Clock::duration::zero()) return out;
  out.code = code_;
  out.remaining = std::chrono::ceil<std::chrono::seconds>(left);
  return out;
}

void ServerRuntime::Disconnect() {
  std::lock_guard lock(mu_);
  if (agent_) {
    agent_->Disconnect();
    return;
  }
  if (!server_) return;
  server_->CloseConnections();
  Log("disconnected by the user at this computer");
}

MediaHostStatus ServerRuntime::Status() const {
  std::lock_guard lock(mu_);
  return media_ ? media_->Status() : MediaHostStatus{};
}

}  // namespace rdm
