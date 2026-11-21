#include "rdmachine/session/media_host.h"

#include <CoreFoundation/CoreFoundation.h>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "rdmachine/audio_control.h"
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
static_assert(kAudioControlMessageType == RDA_MSG_USER_FIRST + 0x13, "an application message type");
static_assert(kClipboardTextMessageType == RDA_MSG_CLIPBOARD_TEXT, "rdaccel's clipboard text message");
static_assert(kClipboardHelloMessageType == RDA_MSG_USER_FIRST + 5 && kClipboardCancelMessageType == RDA_MSG_USER_FIRST + 9,
              "application message types");

const char* AudioStateName(bool session_has_audio, const rda_audio_status& s) {
  if (!session_has_audio) return "off";
  switch (s.state) {
    case RDA_AUDIO_STATE_WAITING_PEER:
    case RDA_AUDIO_STATE_STARTING: return "starting";
    case RDA_AUDIO_STATE_RUNNING: return s.silent ? "silent" : "on";
    case RDA_AUDIO_STATE_PAUSED: return "paused";
    case RDA_AUDIO_STATE_NO_PERMISSION: return "no_permission";
    case RDA_AUDIO_STATE_FAILED: return "failed";
    default: return "off";
  }
}

namespace {

// Bundle identifier of the client application: its own playback is left
// out of the capture, so with client and server on one Mac the sound is not
// captured again.
constexpr const char* kClientBundleId = "org.rdmachine.client";

// Overwrites a secret before it is freed (a plain fill may be optimized away).
void Wipe(std::string& s) {
  volatile char* p = s.data();
  for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
  s.clear();
}

constexpr auto kPendingLoginLifetime = std::chrono::seconds(60);

// UDP for the picture, bound to `local_ip`: the address of this computer the
// client's control connection reached. Bound to every address instead, the
// replies would leave from whichever address the routing table picks for the
// client, and on a computer with several ways out (Wi-Fi and Ethernet, a VPN,
// Tailscale) that may not be the one the client sent to, which then drops
// them: the login works, the picture never comes. Falls back to every address
// when the address cannot be bound (unknown, or IPv6 link-local).
rda_status BindMediaUdp(const std::string& local_ip, uint16_t port, rda_transport** out, std::string* where) {
  if (!local_ip.empty()) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    addrinfo* res = nullptr;
    if (getaddrinfo(local_ip.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
      const int fd = socket(res->ai_family, SOCK_DGRAM, 0);
      if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, res->ai_addr, res->ai_addrlen) == 0 && rda_udp_from_fd(fd, 0, out) == RDA_OK) {
          freeaddrinfo(res);
          *where = local_ip;
          return RDA_OK;  // the transport owns fd now
        }
        close(fd);
      }
      freeaddrinfo(res);
    }
  }
  *where = "every address";
  return rda_udp_bind(port, out);
}

std::string Millis();  // the ime debug log's time, below

InputSourceState FromRda(const rda_input_source_info& i) {
  return InputSourceState{i.id, i.name, i.language, i.input_method != 0, i.ascii_capable != 0};
}

}  // namespace

MediaHost::MediaHost(MediaHostOptions options) : options_(std::move(options)) {
  if (options_.ime) {
    ImeBridge::Events ev;
    ev.on_connection = [this](bool connected) {
      {
        std::lock_guard lock(ime_mu_);
        ime_.OnConnection(connected);
      }
      SendImeState(false);
    };
    ev.on_client = [this](bool active, const std::string& app, uint64_t ctl) {
      if (ime_debug_) {
        Log("ime debug " + Millis() + " field " + (active ? "active " : "inactive ") + app + " ctl=" + std::to_string(ctl));
      }
      {
        std::lock_guard lock(ime_mu_);
        ime_.OnField(active, app, ctl);
      }
      SendImeState(false);
    };
    ev.on_report = [this](const ImeBridge::Report& r) { OnImeReport(r); };
    ev.on_ended = [this](std::optional<uint64_t> mark_id) {
      bool current;
      {
        std::lock_guard lock(ime_mu_);
        current = ime_.OnEnded(mark_id);
        if (current) ime_.state().ended = true;
      }
      if (ime_debug_) {
        Log("ime debug " + Millis() + " ended over there id=" + (mark_id ? std::to_string(*mark_id) : "-") +
            (current ? "" : " (not the composition under way: ignored)"));
      }
      if (!current) return;
      SendImeState(false);
      std::lock_guard lock(ime_mu_);
      ime_.state().ended = false;
    };
    ev.on_late_refusal = [this](const std::string& text) { TypeText(text); };
    options_.ime->SetEvents(std::move(ev));
  }
}

MediaHost::~MediaHost() {
  if (options_.ime) options_.ime->SetEvents({});  // returns once no event runs any more
  EndSession();
}

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

void MediaHost::ExpectLogin(const std::string& username, const std::string& password) {
  if (!options_.type_login || password.empty()) return;
  std::lock_guard lock(login_mu_);
  Wipe(pending_login_.password);
  pending_login_ = PendingLogin{username, password, Clock::now()};
}

bool MediaHost::session_running() {
  std::lock_guard lock(mu_);
  return host_ != nullptr;
}

MediaHostStatus MediaHost::Status() {
  // The menu bar asks on the main thread once a second. mu_ is held for
  // seconds while a session starts or its desktop is rebuilt, and those
  // steps need the main thread's event loop running (a new display's modes
  // arrive through it): waiting here would stall both.
  std::unique_lock lock(mu_, std::try_to_lock);
  if (!lock.owns_lock()) {
    std::lock_guard snap(snapshot_mu_);
    return snapshot_;
  }
  MediaHostStatus s = status_;
  if (host_) {
    s.media_connected = media_connected_;
    s.text_mode = text_mode_;
    s.audio = audio_;
    s.audio_status.struct_size = sizeof(s.audio_status);
    if (rda_host_get_audio_status(host_, &s.audio_status) != RDA_OK) s.audio_status = rda_audio_status{};
    s.stats.struct_size = sizeof(s.stats);
    if (rda_host_get_stats(host_, &s.stats) != RDA_OK) s.stats = rda_host_stats{};
    char enc[64] = {};
    rda_host_encoder_name(host_, enc, sizeof enc);
    s.encoder = enc;
    std::lock_guard in(input_source_mu_);
    s.client_input_source = client_input_source_;
    s.input_source = input_source_;
  }
  if (options_.ime && !options_.login_window) {
    bool in_use;
    {
      std::lock_guard im(ime_mu_);
      in_use = host_ && ime_mode_;
    }
    s.ime = in_use ? "in use" : ImeSourceEnabled() ? "ready" : "not enabled";
  }
  std::lock_guard snap(snapshot_mu_);
  snapshot_ = s;
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
  std::lock_guard lock(self->inject_mu_);
  ++self->input_events_;
  if (ev && ev->kind < 32) self->input_kinds_ |= 1u << ev->kind;
  if (self->injector_ && rda_injector_inject(self->injector_, ev) != RDA_OK) {
    ++self->input_failed_;
    self->input_error_ = rda_last_error();
  }
  self->ImeAfterInput(ev);
}

// A click or a key may have moved the caret: the client hears where it is
// before it types the next character (docs/ime.md).
void MediaHost::ImeAfterInput(const rda_input_event* ev) {
  if (ev && ime_mode_on_ && options_.ime && (ev->kind == RDA_INPUT_MOUSE_UP || ev->kind == RDA_INPUT_KEY_UP)) {
    options_.ime->RequestReport(60);
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
  if (type == kImeMessageType) {
    // In order with the keys (rda_viewer_send_input_message).
    if (auto in = DecodeImeInput(std::string(reinterpret_cast<const char*>(payload), n))) self->OnImeInput(*in);
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
        // The data saver's sound goes with it (docs/audio.md, 11).
        rda_host_set_audio_profile(h, mode->text_mode ? RDA_AUDIO_PROFILE_SAVER : RDA_AUDIO_PROFILE_FULL);
        if (self->text_mode_.exchange(mode->text_mode) != mode->text_mode) {
          self->Log(mode->text_mode ? "text mode on (asked by the client)" : "text mode off (asked by the client)");
        }
      }
    }
  }
  if (type == kAudioControlMessageType) {
    // The client cannot hear it (sound off in its menu, its output muted):
    // nothing is sent meanwhile. This computer stays muted all the same.
    if (auto c = DecodeAudioControl(std::string(reinterpret_cast<const char*>(payload), n))) {
      if (rda_host* h = self->mode_host_.load(); h && self->audio_) {
        rda_audio_status st{};
        st.struct_size = sizeof(st);
        const bool was = rda_host_get_audio_status(h, &st) == RDA_OK && st.state == RDA_AUDIO_STATE_PAUSED;
        rda_host_set_audio_paused(h, c->paused ? 1 : 0);
        if (was != c->paused) self->Log(c->paused ? "sound paused (the client cannot hear it)" : "sound resumed");
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

// Transport thread: never takes mu_. EndSession holds mu_ while rdaccel
// waits for this thread to finish, so waiting for mu_ here would stop both
// for good (and with them every later session of this server).
void MediaHost::SyncInputSource(const InputSourceState& wanted) {
  {
    std::lock_guard lock(ime_mu_);
    if (ime_mode_) {
      // The client types with its own input method: this computer stays on RDMachine Input.
      Log("input source from the client (" + wanted.id + ") ignored: it types with its own input method");
      return;
    }
  }
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
    // At the login window only keyboard layouts (see ChooseInputSource).
    if (auto id = ChooseInputSource(wanted, available, options_.login_window)) {
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
  std::lock_guard lock(input_source_mu_);
  client_input_source_ = wanted.name.empty() ? wanted.id : wanted.name;
  input_source_ = selected;
}

namespace {

// `defaults write org.rdmachine.server ImeDebugLog -bool YES` (read when a
// session starts): every position RDMachine Input reports, and every
// composition message from the client, go to the log with milliseconds (no
// text), to see why a candidate window moves.
bool ImeDebugLogWanted() {
  CFPreferencesAppSynchronize(CFSTR("org.rdmachine.server"));
  Boolean valid = false;
  return CFPreferencesGetAppBooleanValue(CFSTR("ImeDebugLog"), CFSTR("org.rdmachine.server"), &valid) && valid;
}

std::string Millis() {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  char buf[16];
  snprintf(buf, sizeof buf, "%02lld.%03lld", static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000));
  return buf;
}

std::string RangeText(const std::optional<ImeRange>& r) {
  return r ? std::to_string(r->location) + "+" + std::to_string(r->length) : "-";
}

}  // namespace

bool MediaHost::ImeSourceEnabled() {
  size_t count = 0;
  if (rda_list_input_sources(nullptr, 0, &count) != RDA_OK || count == 0) return false;
  std::vector<rda_input_source_info> infos(count);
  infos[0].struct_size = sizeof(rda_input_source_info);
  if (rda_list_input_sources(infos.data(), infos.size(), &count) != RDA_OK) return false;
  infos.resize(std::min(count, infos.size()));
  for (const auto& i : infos) {
    if (std::string(i.id) == kImeInputSourceId) return true;
  }
  return false;
}

namespace {

// The Unicode scalar values of UTF-8 text (malformed bytes are skipped).
std::vector<uint32_t> CodePoints(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<uint8_t>(s[i]);
    const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) {
      ++i;
      continue;
    }
    uint32_t cp = n == 1 ? c : c & (0x7f >> n);
    for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3f);
    out.push_back(cp);
    i += n;
  }
  return out;
}

}  // namespace

void MediaHost::OnImeInput(const ImeInput& in) {
  ImeBridge* bridge = options_.ime.get();
  if (!bridge) return;
  if (ime_debug_) {
    static const char* kKinds[] = {"mark", "commit", "cancel", "mode"};
    Log("ime debug " + Millis() + " client " + kKinds[static_cast<int>(in.kind)] + " seq=" + std::to_string(in.seq) +
        (in.kind == ImeInput::Kind::kMark || in.kind == ImeInput::Kind::kCommit
             ? " len=" + std::to_string(in.text.size()) + " sel=" + std::to_string(in.sel_location) + "+" +
                   std::to_string(in.sel_length)
             : ""));
  }
  {
    std::lock_guard lock(ime_mu_);
    ime_last_input_ = in.seq;
  }
  switch (in.kind) {
    case ImeInput::Kind::kMode: {
      bridge->NoteClientMessage(in.seq);
      if (!in.mode_on) {
        EndImeMode("the client stopped typing with its input method", /*restore=*/true);
        break;
      }
      std::lock_guard src(ime_source_mu_);
      bool available;
      {
        std::lock_guard lock(ime_mu_);
        if (ime_mode_) break;
        available = ime_available_ = ime_available_ || (injector_ && !options_.login_window && ImeSourceEnabled());
      }
      if (!available || !injector_) {
        SendImeState(false);
        break;
      }
      rda_input_source_info cur{};
      cur.struct_size = sizeof(cur);
      const std::string prev = rda_current_input_source(&cur) == RDA_OK ? cur.id : "";
      if (prev != kImeInputSourceId && rda_injector_select_input_source(injector_, kImeInputSourceId) != RDA_OK) {
        Log(std::string("the client's input method: RDMachine Input could not be selected: ") + rda_last_error());
        break;
      }
      {
        std::lock_guard lock(ime_mu_);
        ime_mode_ = true;
        ime_mode_on_ = true;
        if (prev != kImeInputSourceId) ime_prev_source_ = prev;
      }
      Log("the client types with its own input method (this Mac " +
          (prev == kImeInputSourceId ? std::string("already on RDMachine Input, nothing switched)")
                                     : "on RDMachine Input" + (prev.empty() ? std::string(")") : ", was " + prev + ")")));
      SendImeState(false);
      // Where the caret is before the first letter. Nothing else asks now:
      // the field kept the focus (no input source switch, so no activation
      // and its report), and the client sends no keys while it composes.
      bridge->RequestReport(0);
      break;
    }
    case ImeInput::Kind::kMark: {
      // Under the lock with the send: the input method may say the system
      // ended this very composition before Mark() returns.
      std::lock_guard lock(ime_mu_);
      ime_.OnMark(bridge->Mark(in.seq, in.text, in.sel_location, in.sel_length));
      break;
    }
    case ImeInput::Kind::kCommit: {
      {
        std::lock_guard lock(ime_mu_);
        ime_.OnCompositionDone();
      }
      // The keys after it wait here (this is the thread that injects them),
      // so they cannot overtake the text; not longer than 100 ms. Typed only
      // when nothing else will put it in: typed and put in by the input
      // method was the text twice.
      if (bridge->Commit(in.seq, in.text, 100) == ImeBridge::CommitResult::kNotTaken) TypeText(in.text);
      break;
    }
    case ImeInput::Kind::kCancel: {
      std::lock_guard lock(ime_mu_);
      bridge->Cancel(in.seq);
      ime_.OnCompositionDone();
      break;
    }
  }
}

void MediaHost::TypeText(const std::string& text) {
  // RDMachine Input passes keys through, so the characters go into it as they
  // are (rda_injector_set_text_passthrough_source); another input method that
  // has taken its place is stepped away from as for any typed text.
  for (uint32_t cp : CodePoints(text)) {
    rda_input_event ev;
    rda_input_event_default(&ev);
    ev.kind = RDA_INPUT_TEXT;
    ev.code = cp;
    Inject(ev);
  }
}

void MediaHost::EndImeMode(const char* why, bool restore) {
  if (restore && !injector_) return;  // view only: no input source changes, nor forgetting the one to restore
  std::lock_guard src(ime_source_mu_);
  std::string prev;
  {
    std::lock_guard lock(ime_mu_);
    // Not in the mode in this session, but an earlier one left RDMachine
    // Input selected: a client in another mode says so when it connects, and
    // the source from before comes back then.
    if (!ime_mode_ && !(restore && !ime_prev_source_.empty())) return;
    ime_mode_ = false;
    ime_mode_on_ = false;
    if (!restore) {
      // Selecting an input method from this background process does not
      // reliably reach the app in front (macOS activates RDMachine Input
      // there and deactivates it again): every session starting and ending
      // with a switch was a reconnect without a text field (docs/ime.md).
      Log(std::string("the client's input method: off (") + why + "), this Mac stays on RDMachine Input");
      return;
    }
    prev = std::move(ime_prev_source_);
    ime_prev_source_.clear();
  }
  rda_input_source_info cur{};
  cur.struct_size = sizeof(cur);
  std::string outcome;
  if (prev.empty()) {
    outcome = ", no input source from before to put back";
  } else if (!injector_ || rda_current_input_source(&cur) != RDA_OK || cur.id != std::string(kImeInputSourceId)) {
    outcome = ", left as it is (no longer RDMachine Input)";
  } else if (rda_injector_select_input_source(injector_, prev.c_str()) != RDA_OK) {
    outcome = ", " + prev + " could not be selected again: " + rda_last_error();
  } else {
    outcome = ", back to " + prev;
  }
  Log(std::string("the client's input method: off (") + why + ")" + outcome);
}

void MediaHost::OnImeReport(const ImeBridge::Report& r) {
  std::optional<ImeRect> caret;
  if (r.rect) {
    int32_t ox = 0, oy = 0;
    ImeRect px;
    if (GlobalRectToDisplayPixels(*r.rect, &ox, &oy, &px)) {
      size_t count = 0;
      std::vector<rda_display_info> infos;
      if (rda_list_displays(nullptr, 0, &count) == RDA_OK && count > 0) {
        infos.resize(count);
        infos[0].struct_size = sizeof(rda_display_info);
        if (rda_list_displays(infos.data(), infos.size(), &count) != RDA_OK) infos.clear();
        infos.resize(std::min(count, infos.size()));
      }
      std::lock_guard lock(ime_mu_);
      for (const rda_display_info& d : infos) {
        if (d.x != ox || d.y != oy) continue;
        for (size_t i = 0; i < ime_displays_.size(); ++i) {
          if (ime_displays_[i] == d.index) {
            px.display = static_cast<int32_t>(i);
            caret = px;
          }
        }
      }
    }
  }
  if (ime_debug_) {
    char g[96] = "-";
    if (r.rect) snprintf(g, sizeof g, "%.1f,%.1f %.1fx%.1f", r.rect->x, r.rect->y, r.rect->width, r.rect->height);
    char p[96] = "-";
    if (caret) snprintf(p, sizeof p, "d%d %d,%d %dx%d", caret->display, caret->x, caret->y, caret->width, caret->height);
    Log("ime debug " + Millis() + " report seq=" + std::to_string(r.seq) +
        " answers=" + (r.answers ? std::to_string(*r.answers) : "-") + " ctl=" + std::to_string(r.ctl) +
        " marked=" + RangeText(r.marked) + " sel=" + RangeText(r.selected) + " global=" + g + " pixels=" + p +
        (r.active ? (*r.active ? " active " : " inactive ") + r.app : std::string()));
  }
  ImeStateTracker::Report t;
  t.active = r.active;
  t.ctl = r.ctl;
  t.app = r.app;
  t.answers = r.answers;
  t.caret = caret;
  t.marked = r.marked;
  t.selected = r.selected;
  {
    std::lock_guard lock(ime_mu_);
    // A late report of a field that lost the focus (each field has its own
    // controller over there, and its timers outlive the focus) does not count.
    if (!ime_.OnReport(t, ime_last_input_)) return;
  }
  SendImeState(false);
}

void MediaHost::SendImeState(bool force) {
  rda_host* h = mode_host_.load();
  if (!h || !options_.ime) return;
  std::string payload;
  bool first = false;
  bool available = false;
  {
    std::lock_guard lock(ime_mu_);
    ImeState s = ime_.state();
    s.available = ime_available_;
    s.secure = SecureInputActive();
    s.client = s.client && options_.ime->connected();
    if (!force && ime_sent_ && s == ime_last_sent_) return;
    first = !ime_sent_;
    available = s.available;
    ime_last_sent_ = s;
    ime_sent_ = true;
    payload = EncodeImeState(s);
  }
  if (ime_debug_) Log("ime debug " + Millis() + " sent " + payload);
  const rda_status st =
      rda_host_send_message(h, kImeStateMessageType, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  if (first) {
    Log(std::string("the client's input method: ") + (available ? "offered to the client" : "not offered") +
        (st == RDA_OK ? "" : std::string(" (could not send: ") + rda_status_str(st) + ")"));
  }
}

void MediaHost::OnState(void* user, rda_state state, rda_status reason) {
  auto* self = static_cast<MediaHost*>(user);
  switch (state) {
    case RDA_STATE_CONNECTED:
      self->media_connected_ = true;
      self->Log("media connected");
      self->SendImeState(true);  // also tells a new client that this server takes its input method
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
  auto* self = static_cast<MediaHost*>(user);
  const std::string what = message ? message : "";
  self->Log(std::string("rdaccel host: ") + rda_status_str(status) + ": " + what);
  // Capture stopped by the system (lock, display sleep): start it again, on
  // the capture thread (no rdaccel call from inside its own callback).
  if (what.rfind("capture stopped", 0) == 0) {
    {
      std::lock_guard c(self->capture_mu_);
      self->capture_restart_ = true;
    }
    self->capture_cv_.notify_all();
  }
}

void MediaHost::StopCaptureLoop() {
  {
    std::lock_guard c(capture_mu_);
    capture_stop_ = true;
  }
  capture_cv_.notify_all();
  if (capture_thread_.joinable()) capture_thread_.join();
}

void MediaHost::CaptureLoop() {
  int failures = 0;  // in a row: logged at the first and then every minute
  std::unique_lock c(capture_mu_);
  for (;;) {
    capture_cv_.wait(c, [this] { return capture_stop_ || capture_restart_; });
    if (capture_stop_) break;
    capture_restart_ = false;  // a stop reported from here on asks again
    c.unlock();
    bool ok = true;
    {
      std::lock_guard lock(mu_);  // host_ stays alive while it is held
      if (host_) {
        for (auto& x : extras_) {
          if (!x->host) continue;
          rda_host_stop_capture(x->host);
          rda_host_start_capture(x->host);
        }
        rda_host_stop_capture(host_);
        ok = rda_host_start_capture(host_) == RDA_OK;
        if (ok) {
          Log(failures ? "screen capture started again after " + std::to_string(failures) + " tries"
                       : std::string("screen capture started again"));
        } else if (failures % 30 == 0) {
          Log(std::string("screen capture could not start again yet (trying every 2 s): ") +
              rda_last_error());
        }
      }
    }
    failures = ok ? 0 : failures + 1;
    c.lock();
    if (!ok) {
      // The display may still be asleep: try again in a while.
      capture_cv_.wait_for(c, std::chrono::seconds(2), [this] { return capture_stop_; });
      capture_restart_ = true;
    }
  }
}

std::optional<SessionGrant> MediaHost::StartSession(const SessionRequest& request, const SessionSecrets& secrets,
                                                    const SessionOrigin& origin, std::string* error) {
  // The password of this login, if it was one (see type_login).
  PendingLogin login;
  {
    std::lock_guard l(login_mu_);
    if (!pending_login_.password.empty() &&
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

  // An extended desktop: more displays beside this one.
  if (desktop) {
    desktop_ = std::move(desktop);  // (BuildExtraDesktops places them next to it)
    BuildExtraDesktops(request);
    desktop = std::move(desktop_);
  }

  // Displays.
  size_t count = 0;
  if (rda_list_displays(nullptr, 0, &count) != RDA_OK || count == 0) {
    extras_.clear();
    *error = "no_display";
    Log(std::string("no display: ") + rda_last_error());
    return std::nullopt;
  }
  std::vector<rda_display_info> infos(count);
  infos[0].struct_size = sizeof(rda_display_info);
  if (rda_list_displays(infos.data(), infos.size(), &count) != RDA_OK) {
    extras_.clear();
    *error = "no_display";
    return std::nullopt;
  }
  infos.resize(std::min(count, infos.size()));
  if (display_index < 0 || static_cast<size_t>(display_index) >= infos.size()) {
    extras_.clear();
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
    } else {
      // Typed text goes into RDMachine Input as it is (it passes every key
      // through); stepping away from it would leave an ASCII layout in its
      // place, which nothing brings back while only text comes (rdaccel 3.4).
      rda_injector_set_text_passthrough_source(injector, kImeInputSourceId);
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
    // A view-only client's files are not taken either.
    if (clip_mode != ClipboardMode::kSend) {
      so.file_target = options_.file_target;
      so.file_staging_dir = options_.file_staging_dir;
    }
    // Files the client pastes go below the picture (rdaccel's bulk channel).
    so.send_bulk = [this](uint32_t type, std::string_view payload) {
      rda_host* h = clipboard_host_.load();
      return h && rda_host_send_bulk_message(h, type, reinterpret_cast<const uint8_t*>(payload.data()),
                                             payload.size()) == RDA_OK;
    };
    so.bulk_queued = [this]() -> int64_t {
      rda_host* h = clipboard_host_.load();
      return h ? rda_host_bulk_queued_bytes(h) : -1;
    };
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
  grant.port = options_.udp_port;
  std::string media_where = "every address";
  const rda_status ts = BindMediaUdp(origin.local_ip, grant.port, &transport, &media_where);
  if (ts != RDA_OK) {
    Log(std::string("cannot listen for media on port ") + std::to_string(grant.port) + ": " + rda_last_error());
    if (injector) rda_injector_destroy(injector);
    clipboard_.reset();
    extras_.clear();
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
  {
    std::lock_guard in(input_source_mu_);  // before the host exists: nothing of an earlier session's
    client_input_source_.clear();
    input_source_.clear();
  }
  {
    std::lock_guard in(inject_mu_);  // TypeText may run on the input method's thread
    injector_ = injector;
  }
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
    {
      std::lock_guard in(inject_mu_);
      injector_ = nullptr;
    }
    if (injector) rda_injector_destroy(injector);
    clipboard_.reset();
    extras_.clear();
    tokens_.Consume(secrets.auth_token, Clock::now());
    *error = hs == RDA_ERR_PERMISSION ? "capture_denied" : "internal";
    return std::nullopt;
  }
  host_ = host;
  // No display sleep while someone is connected: it would stop the capture.
  awake_ = KeepDisplaysAwake("rdmachine: a remote session is connected");
  text_mode_ = request.text_mode;
  // Sound, for a client that asked (docs/audio.md): rdaccel captures once its
  // viewer is connected and says it takes sound. Not from the login window,
  // not when the server's setting is off; a failure costs the sound only.
  audio_ = false;
  if (request.audio) {
    if (!options_.send_audio) {
      grant.audio_reason = "disabled";
    } else if (options_.login_window) {
      grant.audio_reason = "login_window";
    } else {
      rda_audio_config ac;
      rda_audio_config_default(&ac);
      ac.source = options_.audio_source;
      ac.profile = request.text_mode ? RDA_AUDIO_PROFILE_SAVER : RDA_AUDIO_PROFILE_FULL;
      ac.mute_local = options_.mute_local_audio ? 1 : 0;
      const char* exclude[] = {kClientBundleId};
      ac.exclude_bundle_ids = exclude;
      ac.exclude_bundle_id_count = 1;
      if (rda_host_start_audio(host, &ac) == RDA_OK) {
        audio_ = true;
        grant.audio = true;
      } else {
        Log(std::string("sound unavailable: ") + rda_last_error());
        grant.audio_reason = "unsupported";
      }
    }
  }
  {
    std::lock_guard im(ime_mu_);
    ime_last_sent_ = ImeState{};
    ime_sent_ = false;
    ime_mode_ = false;
    ime_mode_on_ = false;
    ime_displays_ = {display_index};
    // A text field that got the focus before this session: RDMachine Input
    // says so only when that changes. The client numbers its messages from 1
    // again.
    if (options_.ime) {
      ime_.Reset(options_.ime->client_active(), options_.ime->client_app(), options_.ime->client_ctl());
      options_.ime->NewSession();
    } else {
      ime_.Reset(false, "", 0);
    }
    ime_last_input_ = 0;
    ime_debug_ = options_.ime && ImeDebugLogWanted();
    if (ime_debug_) Log("ime debug: on (ImeDebugLog)");
    // Not at the login window (no third-party input method there), not view only.
    ime_available_ = options_.ime && injector && !options_.login_window && ImeSourceEnabled();
    if (options_.ime && injector && !options_.login_window && !ime_available_) {
      Log("the client's input method: RDMachine Input is not enabled in System Settings");
    }
  }
  mode_host_ = host;
  status_ = MediaHostStatus{};
  status_.running = true;
  status_.origin = origin;
  status_.view_only = injector == nullptr;
  status_.input_unavailable = !view_only && injector == nullptr;
  status_.started = std::chrono::steady_clock::now();
  status_.audio = audio_;
  status_.audio_reason = audio_ ? std::string() : request.audio ? grant.audio_reason : std::string("client");
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
  {
    std::lock_guard c(capture_mu_);
    capture_stop_ = false;
    capture_restart_ = false;
  }
  capture_thread_ = std::thread(&MediaHost::CaptureLoop, this);
  {
    std::lock_guard in(inject_mu_);
    input_events_ = input_failed_ = input_logged_ = 0;
    input_kinds_ = 0;
    input_error_.clear();
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
      (request.text_mode ? " (text mode)" : "") + (options_.login_window ? " (login window)" : "") +
      (audio_ ? std::string(", sound") + (options_.mute_local_audio ? " (this Mac silent)" : "")
              : request.audio ? ", no sound (" + grant.audio_reason + ")" : "") +
      ", media " +
      "udp on " + media_where + " port " + std::to_string(grant.port) +
      ", clipboard " + ClipboardModeName(clip_mode));
  StartExtraHosts(request, secrets, origin, injector != nullptr, &grant);
  {
    std::lock_guard im(ime_mu_);
    for (const auto& x : extras_) ime_displays_.push_back(x->display_index);
  }
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

void MediaHost::LogInput() {
  std::string line;
  {
    std::lock_guard in(inject_mu_);
    if (input_events_ == input_logged_) return;
    std::string kinds;
    const char* names[] = {"", "move", "down", "up", "scroll", "key-down", "key-up", "text"};
    for (int k = 1; k < 8; ++k) {
      if (input_kinds_ & (1u << k)) kinds += std::string(kinds.empty() ? "" : ",") + names[k];
    }
    line = "input from the client: " + std::to_string(input_events_) + " events (" + kinds + "), " +
           std::to_string(input_failed_) + " not injected" +
           (input_error_.empty() ? std::string() : ": " + input_error_) + (injector_ ? "" : ", no injector");
    input_logged_ = input_events_;
  }
  Log(line);
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
      // The client's input since the last line, every 5 seconds while some came.
      static thread_local int tick = 0;
      if (++tick % 5 == 0) LogInput();
    }
    // The client's input method: the first state (the picture may connect
    // before mode_host_ is set, and that first message then goes nowhere),
    // and a password field taking secure input or letting it go.
    if (media_connected_) SendImeState(false);
    {
      std::lock_guard lock(mu_);  // host_ stays alive while it is held
      if (!host_) break;
      rda_host_stats hs{};
      hs.struct_size = sizeof(hs);
      if (rda_host_get_stats(host_, &hs) == RDA_OK) {
        const LiveStats ls = sampler.Update(hs, LiveStatsSampler::Clock::now());
        if (ls.valid && media_connected_) {
          SessionStatsReport r;
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
          {
            std::lock_guard in(input_source_mu_);
            r.input_source = input_source_;
          }
          r.view_only = status_.view_only;
          r.text_mode = text_mode_;
          rda_audio_status as{};
          as.struct_size = sizeof(as);
          if (rda_host_get_audio_status(host_, &as) != RDA_OK) as = rda_audio_status{};
          r.audio = AudioStateName(audio_, as);
          r.audio_kbps = ls.audio_kbps;
          // The sound's state changes are logged once each (no permission
          // and failures with their reason).
          if (r.audio != last_audio_state_) {
            if (audio_) {
              Log("sound: " + r.audio + (as.message[0] ? std::string(" (") + as.message + ")" : std::string()) +
                  (as.muted_local ? ", this Mac silent" : ""));
            }
            last_audio_state_ = r.audio;
          }
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
  bool typed = false, unlocked = false;
  if (connected) {
    // A Shift tap wakes a display that is off (the injector also declares
    // user activity) and brings up the password field; give it time.
    key(kHidLeftShift, true, RDA_MOD_SHIFT);
    key(kHidLeftShift, false, 0);
    // Still what it was when the session started? Seconds have passed, and
    // someone at this computer may have unlocked it meanwhile: the password
    // would then go into whatever application is in front.
    const bool waited = wait(std::chrono::milliseconds(1500));
    unlocked = waited && !at_login_window &&
               !(options_.login_screen ? options_.login_screen() : QueryLoginScreen()).screen_locked;
    if (waited && !unlocked) {
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
                  (unlocked ? " (it was unlocked meanwhile)"
                            : connected ? " (the session ended)" : " (the viewer did not connect)"));
}

int MediaHost::OnExtraAuth(void* user, const uint8_t* token, size_t n) {
  auto* x = static_cast<Extra*>(user);
  const bool ok = x->self->tokens_.Consume(std::span<const uint8_t>(token, token ? n : 0), Clock::now());
  x->self->Log("extra display " + std::to_string(x->index + 2) + ": viewer " + (ok ? "admitted" : "refused"));
  return ok ? 1 : 0;
}

void MediaHost::OnExtraInput(void* user, const rda_input_event* ev) {
  auto* x = static_cast<Extra*>(user);
  std::lock_guard lock(x->inject_mu);
  if (x->injector && ev) rda_injector_inject(x->injector, ev);
  x->self->ImeAfterInput(ev);  // a click on that display may move the caret too
}

void MediaHost::OnExtraState(void* user, rda_state state, rda_status reason) {
  auto* x = static_cast<Extra*>(user);
  if (state == RDA_STATE_CONNECTED) x->self->Log("extra display " + std::to_string(x->index + 2) + ": media connected");
  if (state == RDA_STATE_DISCONNECTED) {
    x->self->Log("extra display " + std::to_string(x->index + 2) + ": media disconnected: " + rda_status_str(reason));
  }
}

void MediaHost::OnExtraError(void* user, rda_status status, const char* message) {
  auto* x = static_cast<Extra*>(user);
  const std::string what = message ? message : "";
  x->self->Log("extra display " + std::to_string(x->index + 2) + ": " + rda_status_str(status) + ": " + what);
  if (what.rfind("capture stopped", 0) == 0) {
    {
      std::lock_guard c(x->self->capture_mu_);
      x->self->capture_restart_ = true;
    }
    x->self->capture_cv_.notify_all();
  }
}

void MediaHost::BuildExtraDesktops(const SessionRequest& request) {
  extras_.clear();
  if (!desktop_ || request.extra_desktops.empty()) return;
  if (!options_.extra_desktop) {
    Log("an extended desktop was asked for, but this server cannot build one; one display only");
    return;
  }
  for (size_t i = 0; i < request.extra_desktops.size() && i < kMaxExtraDesktops; ++i) {
    const ExtraDesktop& want = request.extra_desktops[i];
    const std::optional<DesktopSpec> planned = PlanDesktop(want.desktop);
    if (!planned) {
      Log("extra display " + std::to_string(i + 2) + " ignored: not a usable size");
      continue;
    }
    std::string why;
    auto d = options_.extra_desktop(*planned, want.x, want.y, static_cast<int>(i + 2), &why);
    if (!d) {
      Log("extra display " + std::to_string(i + 2) + " could not be built: " + why);
      continue;
    }
    auto x = std::make_unique<Extra>();
    x->self = this;
    x->index = i;
    x->info.desktop = d->spec();
    x->info.x = want.x;
    x->info.y = want.y;
    x->desktop = std::move(d);
    int32_t ox = want.x, oy = want.y;
    x->desktop->Origin(&ox, &oy);
    Log("extra display " + std::to_string(i + 2) + " built: " + FormatDesktopSpec(x->info.desktop) + " at " +
        std::to_string(ox) + "," + std::to_string(oy) + " (asked for " + std::to_string(want.x) + "," +
        std::to_string(want.y) + ")");
    extras_.push_back(std::move(x));
  }
}

void MediaHost::StartExtraHosts(const SessionRequest& request, const SessionSecrets& secrets,
                                const SessionOrigin& origin, bool inject, SessionGrant* grant) {
  if (extras_.empty()) return;
  // rdaccel's numbering of the displays: found by where each one is.
  size_t count = 0;
  std::vector<rda_display_info> infos;
  if (rda_list_displays(nullptr, 0, &count) == RDA_OK && count > 0) {
    infos.resize(count);
    infos[0].struct_size = sizeof(rda_display_info);
    if (rda_list_displays(infos.data(), infos.size(), &count) == RDA_OK) {
      infos.resize(std::min(count, infos.size()));
    } else {
      infos.clear();
    }
  }
  std::vector<std::unique_ptr<Extra>> started;
  for (auto& x : extras_) {
    const std::string name = "extra display " + std::to_string(x->index + 2);
    int32_t ox = 0, oy = 0;
    if (!x->desktop->Origin(&ox, &oy)) continue;
    for (const rda_display_info& d : infos) {
      if (d.x == ox && d.y == oy && d.index != 0) x->display_index = d.index;
    }
    if (x->display_index < 0) {
      Log(name + ": not among the displays that can be captured; left out");
      continue;
    }
    if (inject) {
      rda_injector_config ic;
      rda_injector_config_default(&ic);
      ic.display_index = x->display_index;
      if (rda_injector_create(&ic, &x->injector) != RDA_OK) x->injector = nullptr;
    }
    const uint16_t port = static_cast<uint16_t>(options_.udp_port + 2 + x->index);
    rda_transport* transport = nullptr;
    std::string where = "every address";
    const rda_status ts = BindMediaUdp(origin.local_ip, port, &transport, &where);
    if (ts != RDA_OK) {
      Log(name + ": cannot listen on port " + std::to_string(port) + ": " + rda_last_error());
      if (x->injector) rda_injector_destroy(x->injector);
      x->injector = nullptr;
      continue;
    }
    const SessionSecrets own = ExtraDisplaySecrets(secrets, x->index);
    rda_host_config cfg;
    rda_host_config_default(&cfg);
    if (options_.fps > 0) cfg.fps = options_.fps;
    if (options_.bitrate_bps > 0) cfg.bitrate_bps = options_.bitrate_bps;
    cfg.display_index = x->display_index;
    cfg.encryption_key = own.key.data();  // copied by rdaccel
    cfg.keep_serving = 0;
    cfg.capture_own_windows = 1;
    cfg.text_mode = request.text_mode ? 1 : 0;
    cfg.user = x.get();
    cfg.on_auth = &MediaHost::OnExtraAuth;
    cfg.on_input = &MediaHost::OnExtraInput;
    cfg.on_state = &MediaHost::OnExtraState;
    cfg.on_error = &MediaHost::OnExtraError;
    tokens_.Add(own.auth_token, Clock::now());
    if (rda_host_create(transport, &cfg, &x->host) != RDA_OK) {  // takes the transport
      Log(name + ": rdaccel host failed: " + rda_last_error());
      x->host = nullptr;
    } else if (rda_host_start_capture(x->host) != RDA_OK) {
      Log(name + ": capture failed: " + rda_last_error());
      rda_host_destroy(x->host);
      x->host = nullptr;
    }
    if (!x->host) {
      if (x->injector) rda_injector_destroy(x->injector);
      x->injector = nullptr;
      continue;
    }
    x->info.port = port;
    grant->extra_displays.push_back(x->info);
    Log(name + ": serving rdaccel display " + std::to_string(x->display_index) + " on " +
 "udp " + where + " port " + std::to_string(port) +
        (x->injector ? "" : " (view only)"));
    started.push_back(std::move(x));
  }
  extras_ = std::move(started);  // the others' displays go now
}

void MediaHost::EndSession() {
  // Not under mu_: the stats and desktop threads take mu_ for their work.
  StopTypeLogin();  // it injects through injector_, destroyed below
  LogInput();
  StopStats();
  StopDesktopLoop();
  StopCaptureLoop();
  EndImeMode("the session ends", /*restore=*/false);
  rda_host* host;
  rda_injector* injector;
  std::unique_ptr<VirtualDesktop> desktop;
  std::vector<std::unique_ptr<Extra>> extras;
  {
    std::lock_guard lock(mu_);
    host = host_;
    host_ = nullptr;
    if (!host) return;
    // The extra displays' hosts first (no callback after rda_host_destroy).
    for (auto& x : extras_) {
      if (x->host) rda_host_destroy(x->host);
      x->host = nullptr;
    }
    extras = std::move(extras_);
    extras_.clear();
    status_ = MediaHostStatus{};
    media_connected_ = false;
    if (clipboard_) clipboard_->Stop();  // its thread may be sending through the host
    clipboard_host_ = nullptr;
    mode_host_ = nullptr;
    text_mode_ = false;
    // The sound first: the capture ends and this computer's own sound is
    // back before anything else goes.
    if (audio_) rda_host_stop_audio(host);
    audio_ = false;
    // Destroy the host under the lock so StartSession cannot bind the port
    // before it is free again.
    rda_host_destroy(host);
    {
      std::lock_guard in(inject_mu_);  // TypeText may run on the input method's thread
      injector = injector_;
      injector_ = nullptr;
    }
    clipboard_.reset();  // no on_message after rda_host_destroy
    desktop = std::move(desktop_);
    awake_.reset();
  }
  if (injector) rda_injector_destroy(injector);
  for (auto& x : extras) {
    if (x->injector) rda_injector_destroy(x->injector);
    x->injector = nullptr;
  }
  if (!extras.empty()) {
    extras.clear();  // their displays go before the main one puts the real displays back
    Log("extra displays removed");
  }
  // Last, with nothing capturing or pointing at it any more: the real
  // displays are as they were before the session.
  if (desktop) {
    desktop.reset();
    Log("desktop removed");
  }
  Log("session ended");
}

}  // namespace rdm
