#include "server_runtime.h"

#include "rdmachine/session/system_auth.h"
#include "rdmachine/session/system_clipboard.h"
#include "rdmachine/session/system_files.h"

namespace rdm {

ServerSettings SettingsOverrides::Apply(ServerSettings s) const {
  if (port) s.port = *port;
  if (view_only) s.view_only = *view_only;
  if (clipboard_mode) s.clipboard_mode = *clipboard_mode;
  if (fps) s.fps = *fps;
  if (bitrate_kbps) s.bitrate_kbps = *bitrate_kbps;
  if (send_audio) s.send_audio = *send_audio;
  if (mute_local_audio) s.mute_local_audio = *mute_local_audio;
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
  add(view_only.has_value(), "--view-only");
  add(clipboard_mode.has_value(), "--clipboard");
  add(fps.has_value(), "--fps");
  add(bitrate_kbps.has_value(), "--bitrate-kbps");
  add(send_audio.has_value(), "--audio");
  add(mute_local_audio.has_value(), "--mute-local");
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
  mo.pam_service = options_.pam_service;
  mo.login_window = options_.login_window;
  mo.inject_input = !s.view_only;
  // No one's clipboard at the login window.
  mo.clipboard = options_.login_window ? nullptr : SystemClipboard();
  mo.clipboard_mode = s.clipboard_mode;
  // Files the client copies, pasted in Finder here (not at the login window).
  if (!options_.login_window) {
    mo.file_target = SystemFileTarget();
    mo.file_staging_dir = FileStagingDir();
  }
  mo.virtual_desktop = SystemVirtualDesktop();
  mo.extra_desktop = SystemExtraDesktop();
  mo.fps = s.fps;
  mo.bitrate_bps = static_cast<int64_t>(s.bitrate_kbps) * 1000;
  mo.send_audio = s.send_audio;
  mo.mute_local_audio = s.mute_local_audio;
  mo.audio_source = options_.audio_source;
  // The client's input method (docs/ime.md): one link to RDMachine Input for
  // the life of this process, kept across settings changes. Not at the login
  // window, where no third-party input method runs.
  if (!options_.login_window && !ime_) ime_ = SystemImeBridge(options_.log);
  mo.ime = options_.login_window ? nullptr : ime_;
  mo.input_icon_badge = options_.menu_bar && !options_.login_window;
  mo.log = options_.log;
  if (!options_.test_password.empty() && !agent_mode()) {
    mo.check_password = [pw = options_.test_password](const std::string& user, const std::string& password,
                                                      std::string* error) {
      if (user == CurrentUserName() && password == pw) return true;
      *error = "bad_credentials";
      return false;
    };
  }
  auto media = std::make_unique<MediaHost>(mo);

  if (agent_mode()) {
    AgentHostOptions ao;
    ao.socket_path = options_.agent_socket;
    ao.kind = options_.agent_kind;
    ao.log = options_.log;
    ao.send_settings = options_.agent_kind == "aqua";
    ao.port = s.port;
    auto agent = std::make_unique<AgentHost>(*media, ao);
    agent->Start();
    media_ = std::move(media);
    agent_ = std::move(agent);
    saved_ = saved;
      Log(std::string("running as the ") + options_.agent_kind + " agent of the rdmachine daemon" +
        (s.view_only ? " (view only)" : "") + ", clipboard " + ClipboardModeName(mo.clipboard ? s.clipboard_mode : ClipboardMode::kOff));
    return true;
  }

  ControlServerOptions co;
  co.bind_address = options_.bind_address;
  co.port = s.port;
  auto server = std::make_unique<ControlServer>(*media, identity_, co);
  if (!server->Start(error)) return false;
  media_ = std::move(media);
  server_ = std::move(server);
  saved_ = saved;
  Log("listening on port " + std::to_string(server_->port()) + (s.view_only ? " (view only)" : "") +
      ", clipboard " + ClipboardModeName(s.clipboard_mode) +
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
