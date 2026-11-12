#include "rdmachine/session/media_host.h"

#include <algorithm>
#include <vector>

#include "rdmachine/login_keys.h"
#include "rdmachine/session/live_stats.h"
#include "rdmachine/picture_mode.h"
#include "rdmachine/session/system_auth.h"
#include "rdmachine/session_stats.h"

namespace rdm {

static_assert(kInputSourceMessageType == RDA_MSG_USER_FIRST + 1, "an application message type");
static_assert(kSessionStatsMessageType == RDA_MSG_USER_FIRST + 2, "an application message type");
static_assert(kDesktopRequestMessageType == RDA_MSG_USER_FIRST + 3, "an application message type");
static_assert(kPictureModeMessageType == RDA_MSG_USER_FIRST + 4, "an application message type");
static_assert(kClipboardTextMessageType == RDA_MSG_CLIPBOARD_TEXT, "rdaccel's clipboard text message");
static_assert(kClipboardHelloMessageType == RDA_MSG_USER_FIRST + 5 && kClipboardCancelMessageType == RDA_MSG_USER_FIRST + 9,
              "application message types");

namespace {

// Overwrites a secret before it is freed (a plain fill may be optimized away).
void Wipe(std::string& s) {
  volatile char* p = s.data();
  for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
  s.clear();
}

constexpr auto kPendingLoginLifetime = std::chrono::seconds(60);

InputSourceState FromRda(const rda_input_source_info& i) {
  return InputSourceState{i.id, i.name, i.language, i.input_method != 0, i.ascii_capable != 0};
}

}  // namespace

MediaHost::MediaHost(MediaHostOptions options) : options_(std::move(options)) {}

MediaHost::~MediaHost() { EndSession(); }

void MediaHost::Log(const std::string& line) {
  if (options_.log) options_.log(line);
}

std::string MediaHost::ServerName() { return options_.server_name; }

bool MediaHost::CheckPassword(const std::string& username, const std::string& password, std::string* error) {
  bool ok = false;
  if (options_.check_password) {
    ok = options_.check_password(username, password, error);
  } else if (!options_.login_window && username != CurrentUserName()) {
    // M0 shares the desktop of the user the server runs as (decision D5). At
    // the login window no one is logged in yet: any account may log in.
    *error = "not_console_user";
    Log("login refused: " + username + " is not the logged-in user");
    return false;
  } else {
    std::string detail;
    const std::string service = options_.pam_service.empty() ? DefaultPamService() : options_.pam_service;
    ok = CheckSystemPassword(service, username, password, error, &detail);
    Log("password login for " + username + ": " + (ok ? "ok" : *error + " (" + detail + ")"));
  }
  if (ok && options_.type_login) {
    std::lock_guard lock(login_mu_);
    Wipe(pending_login_.password);
    pending_login_ = PendingLogin{username, password, Clock::now()};
  }
  return ok;
}

bool MediaHost::session_running() {
  std::lock_guard lock(mu_);
  return host_ != nullptr;
}

MediaHostStatus MediaHost::Status() {
  std::lock_guard lock(mu_);
  MediaHostStatus s = status_;
  if (!host_) return s;
  s.media_connected = media_connected_;
  s.text_mode = text_mode_;
  s.stats.struct_size = sizeof(s.stats);
  if (rda_host_get_stats(host_, &s.stats) != RDA_OK) s.stats = rda_host_stats{};
  char enc[64] = {};
  rda_host_encoder_name(host_, enc, sizeof enc);
  s.encoder = enc;
  return s;
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
  if (self->injector_) {
    std::lock_guard lock(self->inject_mu_);
    rda_injector_inject(self->injector_, ev);
  }
}

void MediaHost::Inject(const rda_input_event& ev) {
  std::lock_guard lock(inject_mu_);
  if (injector_) rda_injector_inject(injector_, &ev);
}

void MediaHost::OnMessage(void* user, uint32_t type, const uint8_t* payload, size_t n) {
  auto* self = static_cast<MediaHost*>(user);
  if (ClipboardSync::IsClipboardMessage(type)) {
    if (self->clipboard_) self->clipboard_->OnMessage(type, payload, n);
    return;
  }
  if (type == kInputSourceMessageType) {
    if (auto wanted = DecodeInputSource(std::string(reinterpret_cast<const char*>(payload), n))) {
      self->SyncInputSource(*wanted);
    }
  }
  if (type == kPictureModeMessageType) {
    if (auto mode = DecodePictureMode(std::string(reinterpret_cast<const char*>(payload), n))) {
      if (rda_host* h = self->mode_host_.load()) {
        rda_host_set_text_mode(h, mode->text_mode ? 1 : 0);
        if (self->text_mode_.exchange(mode->text_mode) != mode->text_mode) {
          self->Log(mode->text_mode ? "text mode on (asked by the client)" : "text mode off (asked by the client)");
        }
      }
    }
  }
  if (type == kDesktopRequestMessageType) {
    // Only the latest counts: a window being dragged larger sends several.
    if (auto wanted = DecodeDesktopSpec(std::string(reinterpret_cast<const char*>(payload), n))) {
      {
        std::lock_guard lock(self->desktop_mu_);
        self->desktop_wanted_ = *wanted;
      }
      self->desktop_cv_.notify_all();
    }
  }
}

// Transport thread. The rdaccel input source calls run on the main thread,
// which may be waiting for mu_ (Status()), so mu_ is not held across them.
void MediaHost::SyncInputSource(const InputSourceState& wanted) {
  std::string selected;
  if (!injector_) {
    // View only: the keys do not come from the client, nor should the input source.
    Log("input source from the client (" + wanted.id + ") ignored: this session is view only");
  } else {
    size_t count = 0;
    std::vector<InputSourceState> available;
    if (rda_list_input_sources(nullptr, 0, &count) == RDA_OK && count > 0) {
      std::vector<rda_input_source_info> infos(count);
      infos[0].struct_size = sizeof(rda_input_source_info);
      if (rda_list_input_sources(infos.data(), infos.size(), &count) == RDA_OK) {
        infos.resize(std::min(count, infos.size()));
        for (const auto& i : infos) available.push_back(FromRda(i));
      }
    }
    rda_input_source_info cur{};
    cur.struct_size = sizeof(cur);
    const bool have_cur = rda_current_input_source(&cur) == RDA_OK;
    if (auto id = ChooseInputSource(wanted, available)) {
      for (const auto& a : available) {
        if (a.id == *id) selected = a.name.empty() ? a.id : a.name;
      }
      if (have_cur && *id == cur.id) {
        // Already there (the user at this computer, or an earlier message).
        Log("input source: " + *id + " already selected (client uses " + wanted.id + ")");
      } else if (rda_injector_select_input_source(injector_, id->c_str()) == RDA_OK) {
        Log("input source: " + *id + " (client uses " + wanted.id + ")");
      } else {
        Log("input source " + *id + " could not be selected: " + rda_last_error());
        selected.clear();
      }
    } else {
      Log("input source: nothing here matches the client's " + wanted.id +
          (available.empty() ? std::string(" (cannot list input sources: ") + rda_last_error() + ")" : ""));
    }
  }
  std::lock_guard lock(mu_);
  if (!host_) return;
  status_.client_input_source = wanted.name.empty() ? wanted.id : wanted.name;
  status_.input_source = selected;
}

void MediaHost::OnState(void* user, rda_state state, rda_status reason) {
  auto* self = static_cast<MediaHost*>(user);
  switch (state) {
    case RDA_STATE_CONNECTED:
      self->media_connected_ = true;
      self->Log("media connected");
      break;
    case RDA_STATE_DISCONNECTED:
      self->media_connected_ = false;
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
                                                    const SessionOrigin& origin, std::string* error) {
  // The password of this login, if it was one (see type_login).
  PendingLogin login;
  {
    std::lock_guard l(login_mu_);
    if (origin.method == AuthMethod::kPassword && !pending_login_.password.empty() &&
        pending_login_.username == origin.username && Clock::now() - pending_login_.at < kPendingLoginLifetime) {
      login = pending_login_;
    }
    Wipe(pending_login_.password);
    pending_login_ = PendingLogin{};
  }
  std::lock_guard lock(mu_);
  if (host_) {
    Wipe(login.password);
    *error = "busy";
    return std::nullopt;
  }

  // The desktop the client asked for. It becomes the main display, which is
  // rdaccel's display 0, so everything below finds it there.
  int32_t display_index = request.display_index;
  std::unique_ptr<VirtualDesktop> desktop;
  if (request.desktop) {
    const std::optional<DesktopSpec> planned = PlanDesktop(*request.desktop);
    if (!options_.virtual_desktop) {
      Log("desktop " + FormatDesktopSpec(*request.desktop) + " asked for, but this server cannot build one; " +
          "sharing the display as it is");
    } else if (!planned) {
      Log("desktop request ignored: not a usable size");
    } else {
      std::string why;
      desktop = options_.virtual_desktop(*planned, &why);
      if (desktop) {
        display_index = 0;
        Log("desktop built: " + FormatDesktopSpec(desktop->spec()) + " (asked for " +
            FormatDesktopSpec(*request.desktop) + ")");
      } else {
        Log("desktop " + FormatDesktopSpec(*planned) + " could not be built: " + why +
            "; sharing the display as it is");
      }
    }
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
  if (display_index < 0 || static_cast<size_t>(display_index) >= infos.size()) {
    *error = "no_display";
    return std::nullopt;
  }
  SessionGrant grant;
  grant.login_window = options_.login_window;
  if (desktop) grant.desktop = desktop->spec();
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
    ic.display_index = display_index;
    const rda_status s = rda_injector_create(&ic, &injector);
    if (s != RDA_OK) {
      // Usually the macOS Accessibility permission. The picture still works.
      Log(std::string("input injection unavailable (") + rda_status_str(s) + "): " + rda_last_error() +
          "; this session is view-only");
      injector = nullptr;
    }
  }

  // Clipboard. A view-only client may still copy from this screen.
  ClipboardMode clip_mode = options_.clipboard ? options_.clipboard_mode : ClipboardMode::kOff;
  if (view_only && clip_mode != ClipboardMode::kOff) {
    clip_mode = clip_mode == ClipboardMode::kReceive ? ClipboardMode::kOff : ClipboardMode::kSend;
  }
  if (clip_mode != ClipboardMode::kOff) {
    ClipboardSync::Options so;
    so.mode = clip_mode;
    // Older clients cannot ask at paste time; they get this computer's text
    // as before.
    so.push_text_to_older_peers = true;
    so.log = options_.log;
    // A large paste yields to the picture: it slows down when frames are
    // skipped for congestion.
    so.congestion_events = [this]() -> uint64_t {
      rda_host* h = clipboard_host_.load();
      if (!h) return 0;
      rda_host_stats st{};
      st.struct_size = sizeof st;
      return rda_host_get_stats(h, &st) == RDA_OK ? st.frames_dropped_congestion : 0;
    };
    clipboard_ = std::make_unique<ClipboardSync>(
        options_.clipboard,
        [this](uint32_t type, std::string_view payload) {
          rda_host* h = clipboard_host_.load();
          return h && rda_host_send_message(h, type, reinterpret_cast<const uint8_t*>(payload.data()),
                                            payload.size()) == RDA_OK;
        },
        so);
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
    clipboard_.reset();
    *error = "internal";
    return std::nullopt;
  }

  // Host.
  rda_host_config cfg;
  rda_host_config_default(&cfg);
  if (options_.fps > 0) cfg.fps = options_.fps;
  if (options_.bitrate_bps > 0) cfg.bitrate_bps = options_.bitrate_bps;
  cfg.display_index = display_index;
  cfg.encryption_key = secrets.key.data();  // copied by rdaccel
  cfg.keep_serving = 0;                      // one viewer per session
  // The server's own menu bar menu, statistics panel and dialogs are part
  // of the desktop being shared: without this macOS capture leaves them
  // out, and a remote user clicks into a menu they cannot see.
  cfg.capture_own_windows = 1;
  cfg.text_mode = request.text_mode ? 1 : 0;
  cfg.user = this;
  cfg.on_auth = &MediaHost::OnAuth;
  cfg.on_input = &MediaHost::OnInput;
  cfg.on_state = &MediaHost::OnState;
  cfg.on_error = &MediaHost::OnError;
  cfg.on_message = &MediaHost::OnMessage;
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
    clipboard_.reset();
    tokens_.Consume(secrets.auth_token, Clock::now());
    *error = hs == RDA_ERR_PERMISSION ? "capture_denied" : "internal";
    return std::nullopt;
  }
  host_ = host;
  text_mode_ = request.text_mode;
  mode_host_ = host;
  status_ = MediaHostStatus{};
  status_.running = true;
  status_.origin = origin;
  status_.transport = grant.transport;
  status_.view_only = injector == nullptr;
  status_.input_unavailable = !view_only && injector == nullptr;
  status_.started = std::chrono::steady_clock::now();
  status_.width = infos[static_cast<size_t>(display_index)].width;
  status_.height = infos[static_cast<size_t>(display_index)].height;
  if (desktop) {
    status_.desktop = desktop->spec();
    status_.width = desktop->spec().width;
    status_.height = desktop->spec().height;
    desktop_ = std::move(desktop);
    {
      std::lock_guard d(desktop_mu_);
      desktop_stop_ = false;
      desktop_wanted_.reset();
    }
    desktop_thread_ = std::thread(&MediaHost::DesktopLoop, this);
  }
  media_connected_ = false;
  {
    std::lock_guard s(stats_mu_);
    stats_stop_ = false;
  }
  stats_thread_ = std::thread(&MediaHost::StatsLoop, this);
  if (clipboard_) {
    clipboard_host_ = host;
    clipboard_->Start();
  }
  Log("session started on display " + std::to_string(display_index) + (view_only ? " (view only)" : "") +
      (request.text_mode ? " (text mode)" : "") + (options_.login_window ? " (login window)" : "") + ", media " +
      (grant.transport == MediaTransport::kTcp ? "tcp" : "udp") + " port " + std::to_string(grant.port) +
      ", clipboard " + ClipboardModeName(clip_mode));
  if (!login.password.empty()) {
    const LoginScreenState ls = options_.login_screen ? options_.login_screen() : QueryLoginScreen();
    if (injector && (options_.login_window || ls.screen_locked)) {
      {
        std::lock_guard t(type_mu_);
        type_stop_ = false;
      }
      type_thread_ = std::thread(&MediaHost::TypeLogin, this, login.username, login.password,
                                 options_.login_window && ls.asks_for_name, options_.login_window);
    }
    Wipe(login.password);
  }
  return grant;
}

void MediaHost::StopStats() {
  {
    std::lock_guard s(stats_mu_);
    stats_stop_ = true;
  }
  stats_cv_.notify_all();
  if (stats_thread_.joinable()) stats_thread_.join();
}

void MediaHost::StatsLoop() {
  LiveStatsSampler sampler;
  std::unique_lock s(stats_mu_);
  while (!stats_cv_.wait_for(s, std::chrono::seconds(1), [this] { return stats_stop_; })) {
    s.unlock();
    {
      std::lock_guard lock(mu_);  // host_ stays alive while it is held
      if (!host_) break;
      rda_host_stats hs{};
      hs.struct_size = sizeof(hs);
      if (rda_host_get_stats(host_, &hs) == RDA_OK) {
        const LiveStats ls = sampler.Update(hs, LiveStatsSampler::Clock::now());
        if (ls.valid && media_connected_) {
          SessionStatsReport r;
          r.transport = status_.transport == MediaTransport::kTcp ? "tcp" : "udp";
          r.width = status_.width;
          r.height = status_.height;
          r.encoded_fps = ls.encoded_fps;
          r.captured_fps = ls.captured_fps;
          r.bitrate_kbps = ls.bitrate_kbps;
          r.target_kbps = ls.target_kbps;
          r.bandwidth_kbps = ls.bandwidth_kbps;
          r.rtt_ms = ls.rtt_ms;
          r.recent_loss_pct = ls.recent_loss_pct;
          r.total_loss_pct = ls.total_loss_pct;
          r.frames_dropped_congestion = ls.frames_dropped_congestion;
          char enc[64] = {};
          rda_host_encoder_name(host_, enc, sizeof enc);
          r.encoder = enc;
          r.input_source = status_.input_source;
          r.view_only = status_.view_only;
          r.text_mode = text_mode_;
          const std::string payload = EncodeSessionStats(r);
          // Before the viewer is connected this is RDA_ERR_BUSY: try next time.
          rda_host_send_message(host_, kSessionStatsMessageType, reinterpret_cast<const uint8_t*>(payload.data()),
                                payload.size());
        }
      }
    }
    s.lock();
  }
}

void MediaHost::StopDesktopLoop() {
  {
    std::lock_guard d(desktop_mu_);
    desktop_stop_ = true;
  }
  desktop_cv_.notify_all();
  if (desktop_thread_.joinable()) desktop_thread_.join();
}

void MediaHost::DesktopLoop() {
  std::unique_lock d(desktop_mu_);
  for (;;) {
    desktop_cv_.wait(d, [this] { return desktop_stop_ || desktop_wanted_; });
    if (desktop_stop_) break;
    const DesktopSpec wanted = *desktop_wanted_;
    desktop_wanted_.reset();
    d.unlock();
    {
      std::lock_guard lock(mu_);  // host_ and desktop_ stay alive while it is held
      const std::optional<DesktopSpec> planned = PlanDesktop(wanted);
      if (host_ && desktop_ && planned && *planned != desktop_->spec()) {
        // Capture reads the display's size when it starts, and a new size is
        // a new stream to the viewer (on_stream_info); the injector and the
        // cursor follow the display by themselves.
        rda_host_stop_capture(host_);
        std::string why;
        const bool ok = desktop_->Resize(*planned, &why);
        const DesktopSpec now = desktop_->spec();
        status_.desktop = now;
        status_.width = now.width;
        status_.height = now.height;
        Log(ok ? "desktop resized: " + FormatDesktopSpec(now)
               : "desktop could not be resized to " + FormatDesktopSpec(*planned) + ": " + why + "; it stays " +
                     FormatDesktopSpec(now));
        if (rda_host_start_capture(host_) != RDA_OK) {
          Log(std::string("capture failed after the desktop changed: ") + rda_last_error());
        }
      }
    }
    d.lock();
  }
}

void MediaHost::StopTypeLogin() {
  {
    std::lock_guard t(type_mu_);
    type_stop_ = true;
  }
  type_cv_.notify_all();
  if (type_thread_.joinable()) type_thread_.join();
}

void MediaHost::TypeLogin(std::string username, std::string password, bool type_name, bool at_login_window) {
  const char* where = at_login_window ? "login window" : "lock screen";
  // Waits up to `d`; false when the session is ending.
  auto wait = [this](std::chrono::milliseconds d) {
    std::unique_lock t(type_mu_);
    return !type_cv_.wait_for(t, d, [this] { return type_stop_; });
  };
  auto key = [this](uint32_t usage, bool down, uint32_t mods) {
    rda_input_event ev;
    rda_input_event_default(&ev);
    ev.kind = down ? RDA_INPUT_KEY_DOWN : RDA_INPUT_KEY_UP;
    ev.code = usage;
    ev.modifiers = mods;
    Inject(ev);
  };
  // The viewer first, so the remote user sees it happen (and could stop it
  // by closing the window).
  bool connected = false;
  for (int i = 0; i < 100 && !connected; ++i) {
    if (!wait(std::chrono::milliseconds(100))) break;
    connected = media_connected_;
  }
  bool typed = false;
  if (connected) {
    // A Shift tap wakes a display that is off (the injector also declares
    // user activity) and brings up the password field; give it time.
    key(kHidLeftShift, true, RDA_MOD_SHIFT);
    key(kHidLeftShift, false, 0);
    if (wait(std::chrono::milliseconds(1500))) {
      std::vector<LoginKey> keys = LoginKeystrokes(username, password, type_name);
      typed = true;
      for (const LoginKey& k : keys) {
        if (!wait(std::chrono::milliseconds(25))) {
          typed = false;
          break;
        }
        if (k.kind == LoginKey::Kind::kText) {
          rda_input_event ev;
          rda_input_event_default(&ev);
          ev.kind = RDA_INPUT_TEXT;
          ev.code = k.code;
          Inject(ev);
        } else if (k.shift) {
          key(kHidLeftShift, true, RDA_MOD_SHIFT);
          key(k.code, true, RDA_MOD_SHIFT);
          key(k.code, false, RDA_MOD_SHIFT);
          key(kHidLeftShift, false, 0);
        } else {
          key(k.code, true, 0);
          key(k.code, false, 0);
        }
      }
      for (LoginKey& k : keys) k.code = 0;
    }
  }
  Wipe(password);
  Log(typed ? std::string("typed the login of ") + username + " into the " + where
            : std::string("did not type the login into the ") + where +
                  (connected ? " (the session ended)" : " (the viewer did not connect)"));
}

void MediaHost::EndSession() {
  // Not under mu_: the stats and desktop threads take mu_ for their work.
  StopTypeLogin();  // it injects through injector_, destroyed below
  StopStats();
  StopDesktopLoop();
  rda_host* host;
  rda_injector* injector;
  std::unique_ptr<VirtualDesktop> desktop;
  {
    std::lock_guard lock(mu_);
    host = host_;
    host_ = nullptr;
    if (!host) return;
    status_ = MediaHostStatus{};
    media_connected_ = false;
    if (clipboard_) clipboard_->Stop();  // its thread may be sending through the host
    clipboard_host_ = nullptr;
    mode_host_ = nullptr;
    text_mode_ = false;
    // Destroy the host under the lock so StartSession cannot bind the port
    // before it is free again.
    rda_host_destroy(host);
    injector = injector_;
    injector_ = nullptr;
    clipboard_.reset();  // no on_message after rda_host_destroy
    desktop = std::move(desktop_);
  }
  if (injector) rda_injector_destroy(injector);
  // Last, with nothing capturing or pointing at it any more: the real
  // displays are as they were before the session.
  if (desktop) {
    desktop.reset();
    Log("desktop removed");
  }
  Log("session ended");
}

}  // namespace rdm
