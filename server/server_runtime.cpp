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
  mo.inject_input = !s.view_only;
  mo.clipboard = SystemClipboard();
  mo.clipboard_mode = s.clipboard_mode;
  mo.fps = s.fps;
  mo.bitrate_bps = static_cast<int64_t>(s.bitrate_kbps) * 1000;
  mo.log = options_.log;
  auto media = std::make_unique<MediaHost>(mo);

  ControlServerOptions co;
  co.bind_address = options_.bind_address;
  co.port = s.port;
  co.offer_pairing_codes = s.offer_pairing_codes;
  auto server = std::make_unique<ControlServer>(*media, identity_, co);
  if (!server->Start(error)) return false;
  media_ = std::move(media);
  server_ = std::move(server);
  saved_ = saved;
  code_.clear();
  Log("listening on port " + std::to_string(server_->port()) + (s.view_only ? " (view only)" : "") +
      ", clipboard " + ClipboardModeName(s.clipboard_mode) + (s.offer_pairing_codes ? "" : ", no pairing codes"));
  return true;
}

bool ServerRuntime::SetShowStatsOverlay(bool show, std::string* error) {
  std::lock_guard lock(mu_);
  ServerSettings s = saved_;
  s.show_stats_overlay = show;
  if (!SaveServerSettings(options_.config_dir, s, error)) return false;
  saved_ = s;
  return true;
}

void ServerRuntime::Stop() {
  std::lock_guard lock(mu_);
  StopLocked();
}

void ServerRuntime::StopLocked() {
  if (server_) server_->Stop();  // ends the session through the delegate
  server_.reset();
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
  return server_ != nullptr;
}

uint16_t ServerRuntime::port() const {
  std::lock_guard lock(mu_);
  return server_ ? server_->port() : 0;
}

std::string ServerRuntime::IssuePairingCode() {
  std::lock_guard lock(mu_);
  if (!server_) return {};
  code_ = server_->IssuePairingCode();
  return code_;
}

ServerRuntime::PairingCode ServerRuntime::CurrentPairingCode() const {
  std::lock_guard lock(mu_);
  PairingCode out;
  if (!server_ || code_.empty()) return out;
  const auto left = server_->PairingCodeRemaining();
  if (left <= Clock::duration::zero()) return out;
  out.code = code_;
  out.remaining = std::chrono::ceil<std::chrono::seconds>(left);
  return out;
}

void ServerRuntime::Disconnect() {
  std::lock_guard lock(mu_);
  if (!server_) return;
  server_->CloseConnections();
  Log("disconnected by the user at this computer");
}

MediaHostStatus ServerRuntime::Status() const {
  std::lock_guard lock(mu_);
  return media_ ? media_->Status() : MediaHostStatus{};
}

}  // namespace rdm
