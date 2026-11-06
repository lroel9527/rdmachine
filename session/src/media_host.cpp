#include "rdmachine/session/media_host.h"

#include <vector>

#include "rdmachine/session/system_auth.h"

namespace rdm {

MediaHost::MediaHost(MediaHostOptions options) : options_(std::move(options)) {}

MediaHost::~MediaHost() { EndSession(); }

void MediaHost::Log(const std::string& line) {
  if (options_.log) options_.log(line);
}

std::string MediaHost::ServerName() { return options_.server_name; }

bool MediaHost::CheckPassword(const std::string& username, const std::string& password, std::string* error) {
  if (options_.check_password) return options_.check_password(username, password, error);
  // M0 shares the desktop of the user the server runs as (decision D5).
  if (username != CurrentUserName()) {
    *error = "not_console_user";
    Log("login refused: " + username + " is not the logged-in user");
    return false;
  }
  std::string detail;
  const std::string service = options_.pam_service.empty() ? DefaultPamService() : options_.pam_service;
  const bool ok = CheckSystemPassword(service, username, password, error, &detail);
  Log("password login for " + username + ": " + (ok ? "ok" : *error + " (" + detail + ")"));
  return ok;
}

bool MediaHost::session_running() {
  std::lock_guard lock(mu_);
  return host_ != nullptr;
}

int MediaHost::OnAuth(void* user, const uint8_t* token, size_t n) {
  auto* self = static_cast<MediaHost*>(user);
  const bool ok = self->tokens_.Consume(std::span<const uint8_t>(token, token ? n : 0), Clock::now());
  self->Log(ok ? "viewer admitted" : "viewer refused: wrong or expired token");
  return ok ? 1 : 0;
}

void MediaHost::OnInput(void* user, const rda_input_event* ev) {
  // injector_ is set before the host exists and cleared after it is
  // destroyed (no callback runs after rda_host_destroy), so no lock here.
  auto* self = static_cast<MediaHost*>(user);
  if (self->injector_) rda_injector_inject(self->injector_, ev);
}

void MediaHost::OnState(void* user, rda_state state, rda_status reason) {
  auto* self = static_cast<MediaHost*>(user);
  switch (state) {
    case RDA_STATE_CONNECTED:
      self->Log("media connected");
      break;
    case RDA_STATE_DISCONNECTED:
      self->Log(std::string("media disconnected: ") + rda_status_str(reason));
      break;
    default:
      break;
  }
}

void MediaHost::OnError(void* user, rda_status status, const char* message) {
  static_cast<MediaHost*>(user)->Log(std::string("rdaccel host: ") + rda_status_str(status) + ": " +
                                     (message ? message : ""));
}

std::optional<SessionGrant> MediaHost::StartSession(const SessionRequest& request, const SessionSecrets& secrets,
                                                    std::string* error) {
  std::lock_guard lock(mu_);
  if (host_) {
    *error = "busy";
    return std::nullopt;
  }

  // Displays.
  size_t count = 0;
  if (rda_list_displays(nullptr, 0, &count) != RDA_OK || count == 0) {
    *error = "no_display";
    Log(std::string("no display: ") + rda_last_error());
    return std::nullopt;
  }
  std::vector<rda_display_info> infos(count);
  infos[0].struct_size = sizeof(rda_display_info);
  if (rda_list_displays(infos.data(), infos.size(), &count) != RDA_OK) {
    *error = "no_display";
    return std::nullopt;
  }
  infos.resize(std::min(count, infos.size()));
  if (request.display_index < 0 || static_cast<size_t>(request.display_index) >= infos.size()) {
    *error = "no_display";
    return std::nullopt;
  }
  SessionGrant grant;
  for (const rda_display_info& d : infos) {
    // rdaccel reports pixels; the scale is not exposed yet (M0 reports 1).
    grant.displays.push_back(DisplayInfo{d.index, d.name, d.width, d.height, 1.0});
  }

  // Input.
  const bool view_only = request.view_only || !options_.inject_input;
  rda_injector* injector = nullptr;
  if (!view_only) {
    rda_injector_config ic;
    rda_injector_config_default(&ic);
    ic.display_index = request.display_index;
    const rda_status s = rda_injector_create(&ic, &injector);
    if (s != RDA_OK) {
      // Usually the macOS Accessibility permission. The picture still works.
      Log(std::string("input injection unavailable (") + rda_status_str(s) + "): " + rda_last_error() +
          "; this session is view-only");
      injector = nullptr;
    }
  }

  // Transport.
  rda_transport* transport = nullptr;
  grant.transport = request.transport;
  grant.port = request.transport == MediaTransport::kTcp ? options_.tcp_port : options_.udp_port;
  const rda_status ts = request.transport == MediaTransport::kTcp ? rda_tcp_bind(grant.port, &transport)
                                                                  : rda_udp_bind(grant.port, &transport);
  if (ts != RDA_OK) {
    Log(std::string("cannot listen for media on port ") + std::to_string(grant.port) + ": " + rda_last_error());
    if (injector) rda_injector_destroy(injector);
    *error = "internal";
    return std::nullopt;
  }

  // Host.
  rda_host_config cfg;
  rda_host_config_default(&cfg);
  if (options_.fps > 0) cfg.fps = options_.fps;
  if (options_.bitrate_bps > 0) cfg.bitrate_bps = options_.bitrate_bps;
  cfg.display_index = request.display_index;
  cfg.encryption_key = secrets.key.data();  // copied by rdaccel
  cfg.keep_serving = 0;                      // one viewer per session
  cfg.user = this;
  cfg.on_auth = &MediaHost::OnAuth;
  cfg.on_input = &MediaHost::OnInput;
  cfg.on_state = &MediaHost::OnState;
  cfg.on_error = &MediaHost::OnError;
  tokens_.Add(secrets.auth_token, Clock::now());
  injector_ = injector;
  rda_host* host = nullptr;
  rda_status hs = rda_host_create(transport, &cfg, &host);  // takes the transport
  if (hs == RDA_OK) {
    hs = rda_host_start_capture(host);
    if (hs != RDA_OK) {
      Log(std::string("capture failed: ") + rda_last_error());
      rda_host_destroy(host);
      host = nullptr;
    }
  } else {
    Log(std::string("rdaccel host failed: ") + rda_last_error());
  }
  if (!host) {
    injector_ = nullptr;
    if (injector) rda_injector_destroy(injector);
    tokens_.Consume(secrets.auth_token, Clock::now());
    *error = hs == RDA_ERR_PERMISSION ? "capture_denied" : "internal";
    return std::nullopt;
  }
  host_ = host;
  Log("session started on display " + std::to_string(request.display_index) + (view_only ? " (view only)" : "") +
      ", media " + (grant.transport == MediaTransport::kTcp ? "tcp" : "udp") + " port " + std::to_string(grant.port));
  return grant;
}

void MediaHost::EndSession() {
  rda_host* host;
  rda_injector* injector;
  {
    std::lock_guard lock(mu_);
    host = host_;
    host_ = nullptr;
    if (!host) return;
    // Destroy the host under the lock so StartSession cannot bind the port
    // before it is free again.
    rda_host_destroy(host);
    injector = injector_;
    injector_ = nullptr;
  }
  if (injector) rda_injector_destroy(injector);
  Log("session ended");
}

}  // namespace rdm
