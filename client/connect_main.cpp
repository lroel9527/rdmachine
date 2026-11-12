// rdmachine-connect: command-line client for testing a server without the
// window: logs in, receives the picture, prints what arrives, and quits.
//
//   rdmachine-connect HOST[:PORT] [--user=NAME | --code=DIGITS] [--accept-new-host]
//                     [--tcp] [--view-only] [--display=N] [--frames=N] [--seconds=N]
//                     [--known-hosts=PATH] [--clipboard=both|send|receive|off] [--click=X,Y]
//                     [--save-frame=PATH.bmp] [--send-input-source=JSON]
//                     [--desktop=WxH[@SCALE]] [--resize-desktop=WxH[@SCALE]] [--move=X,Y]
//                     [--text-mode] [--set-text-mode=on|off]
//
// The password comes from $RDMACHINE_PASSWORD, or is asked on the terminal.
// A first-time server fingerprint is shown and must be confirmed (or
// --accept-new-host); it is then pinned in the known-hosts file. Exits 0
// once --frames pictures arrived (default 1), 1 on failure or timeout.
// On macOS the clipboard is shared while connected (--clipboard, default
// both); other systems have no clipboard backend yet.
//
// --click=X,Y moves the server's pointer to pixel (X, Y) of the shared
// display and clicks the left button once the pictures arrived, then waits
// two seconds: for checking input end to end without the window.
// --move=X,Y only moves the pointer there (no click) and waits two seconds.
// --save-frame=PATH.bmp writes the last picture received (after the
// click, if any): what a client would show.
// --send-input-source=JSON sends a keyboard input source sync message (as
// the window client does when the input source changes) once connected.
// --desktop=2880x1800@2 asks the server for a desktop of that many pixels at
// that scale (pixels per point) instead of its display as it is: what the
// window client does with its window's size. --resize-desktop asks for
// another one once the pictures arrived (the window was resized) and waits
// for the picture to take that size; it fails if it does not.
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <rdaccel/rdaccel.h>

#include "rdmachine/desktop.h"
#include "rdmachine/input_source_sync.h"
#include "rdmachine/picture_mode.h"
#include "rdmachine/protocol.h"
#include "rdmachine/session/config_store.h"
#include "rdmachine/session/remote_session.h"
#include "rdmachine/session/system_clipboard.h"

namespace {

struct Stats {
  std::atomic<int> frames{0};
  std::atomic<int> width{0}, height{0};
  std::atomic<bool> ended{false};
  // --save-frame: the latest picture, tightly packed BGRA.
  bool keep_last = false;
  std::mutex mu;
  std::vector<uint8_t> last;
  int32_t last_w = 0, last_h = 0;
};

// A 32-bit BMP, top-down: readable by Preview and `sips` with no library.
bool WriteBmp(const std::string& path, const std::vector<uint8_t>& bgra, int32_t w, int32_t h) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t pixels = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4, offset = 14 + 40;
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  std::fwrite("BM", 1, 2, f);
  u32(offset + pixels); u32(0); u32(offset);
  u32(40); u32(static_cast<uint32_t>(w)); u32(static_cast<uint32_t>(-h)); u16(1); u16(32);
  u32(0); u32(pixels); u32(2835); u32(2835); u32(0); u32(0);
  const bool ok = std::fwrite(bgra.data(), 1, pixels, f) == pixels;
  return std::fclose(f) == 0 && ok;
}

void OnStreamInfo(void* user, int32_t w, int32_t h) {
  auto* s = static_cast<Stats*>(user);
  s->width = w;
  s->height = h;
}

void OnFrame(void* user, const rda_image* img, const rda_rect*, size_t, const rda_frame_info*) {
  auto* s = static_cast<Stats*>(user);
  if (s->keep_last && img && img->data) {
    std::lock_guard lock(s->mu);
    s->last_w = img->width;
    s->last_h = img->height;
    s->last.resize(static_cast<size_t>(img->width) * img->height * 4);
    for (int32_t y = 0; y < img->height; ++y)
      std::memcpy(s->last.data() + static_cast<size_t>(y) * img->width * 4, img->data + static_cast<size_t>(y) * img->stride,
                  static_cast<size_t>(img->width) * 4);
  }
  ++s->frames;
}

bool Flag(const char* arg, const char* name, std::string* value) {
  const size_t n = std::strlen(name);
  if (std::strncmp(arg, name, n) != 0) return false;
  if (arg[n] == '\0' && !value) return true;
  if (arg[n] == '=' && value) {
    *value = arg + n + 1;
    return true;
  }
  return false;
}

int Usage() {
  std::fprintf(stderr,
               "usage: rdmachine-connect HOST[:PORT] [--user=NAME | --code=DIGITS] [--accept-new-host]\n"
               "                         [--tcp] [--view-only] [--display=N] [--frames=N] [--seconds=N]\n"
               "                         [--known-hosts=PATH] [--clipboard=both|send|receive|off] [--click=X,Y] [--save-frame=PATH] [--send-input-source=JSON]\n"
               "                         [--desktop=WxH[@SCALE]] [--resize-desktop=WxH[@SCALE]] [--move=X,Y]\n"
               "                         [--text-mode] [--set-text-mode=on|off]\n");
  return 2;
}

// "host", "host:port", "[v6]:port" or a bare IPv6 address.
bool SplitHostPort(const std::string& s, std::string* host, uint16_t* port) {
  std::string p;
  if (!s.empty() && s[0] == '[') {
    const auto close = s.find(']');
    if (close == std::string::npos) return false;
    *host = s.substr(1, close - 1);
    if (close + 1 < s.size()) {
      if (s[close + 1] != ':') return false;
      p = s.substr(close + 2);
    }
  } else if (std::count(s.begin(), s.end(), ':') == 1) {
    *host = s.substr(0, s.find(':'));
    p = s.substr(s.find(':') + 1);
  } else {
    *host = s;
  }
  if (!p.empty()) {
    const int v = std::atoi(p.c_str());
    if (v <= 0 || v > 65535) return false;
    *port = static_cast<uint16_t>(v);
  }
  return !host->empty();
}

}  // namespace

int main(int argc, char** argv) {
  rdm::RemoteSession::Options opts;
  rdm::ClientOptions& c = opts.control;
  std::string target, user, code, known_hosts_path = rdm::DefaultConfigDir() + "/known_hosts", v;
  bool accept_new = false;
  int want_frames = 1, seconds = 20;
  int click_x = -1, click_y = -1;
  bool move_only = false;
  std::string save_frame, input_source_json;
  std::optional<rdm::DesktopSpec> resize_desktop;
  std::optional<bool> set_text_mode;  // --set-text-mode: change it once frames arrive
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (Flag(a, "--user", &v)) user = v;
    else if (Flag(a, "--code", &v)) code = v;
    else if (Flag(a, "--accept-new-host", nullptr)) accept_new = true;
    else if (Flag(a, "--tcp", nullptr)) c.request.transport = rdm::MediaTransport::kTcp;
    else if (Flag(a, "--view-only", nullptr)) c.request.view_only = true;
    else if (Flag(a, "--text-mode", nullptr)) c.request.text_mode = true;
    else if (Flag(a, "--set-text-mode", &v)) {
      if (v != "on" && v != "off") return Usage();
      set_text_mode = v == "on";
    }
    else if (Flag(a, "--display", &v)) c.request.display_index = std::atoi(v.c_str());
    else if (Flag(a, "--frames", &v)) want_frames = std::atoi(v.c_str());
    else if (Flag(a, "--seconds", &v)) seconds = std::atoi(v.c_str());
    else if (Flag(a, "--known-hosts", &v)) known_hosts_path = v;
    else if (Flag(a, "--save-frame", &v)) save_frame = v;
    else if (Flag(a, "--desktop", &v)) {
      if (!(c.request.desktop = rdm::ParseDesktopSpec(v))) return Usage();
    }
    else if (Flag(a, "--resize-desktop", &v)) {
      if (!(resize_desktop = rdm::ParseDesktopSpec(v))) return Usage();
    }
    else if (Flag(a, "--send-input-source", &v)) {
      if (!rdm::DecodeInputSource(v)) return Usage();
      input_source_json = v;
    }
    else if (Flag(a, "--move", &v)) {
      if (std::sscanf(v.c_str(), "%d,%d", &click_x, &click_y) != 2 || click_x < 0 || click_y < 0) return Usage();
      move_only = true;
    }
    else if (Flag(a, "--click", &v)) {
      if (std::sscanf(v.c_str(), "%d,%d", &click_x, &click_y) != 2 || click_x < 0 || click_y < 0) return Usage();
    }
    else if (Flag(a, "--clipboard", &v)) {
      auto m = rdm::ParseClipboardMode(v);
      if (!m) return Usage();
      opts.clipboard_mode = *m;
    }
    else if (Flag(a, "--version", nullptr)) {
      std::printf("rdmachine-connect %s (protocol %d, rdaccel %s)\n", rdm::VersionString(), rdm::kProtocolVersion,
                  rda_version_string());
      return 0;
    } else if (a[0] != '-' && target.empty()) target = a;
    else return Usage();
  }
  if (target.empty() || !SplitHostPort(target, &c.host, &c.port) || (!user.empty() && !code.empty())) return Usage();

  if (!code.empty()) {
    c.auth.method = rdm::AuthMethod::kPairingCode;
    c.auth.code = code;
  } else {
    if (user.empty()) {
      const char* u = std::getenv("USER");
      user = u ? u : "";
    }
    c.auth.username = user;
    if (const char* p = std::getenv("RDMACHINE_PASSWORD")) {
      c.auth.password = p;
    } else {
      const std::string prompt = "Password for " + user + "@" + c.host + ": ";
      const char* typed = getpass(prompt.c_str());
      c.auth.password = typed ? typed : "";
    }
  }

  char hostname[256] = {};
  gethostname(hostname, sizeof hostname - 1);
  c.client_name = hostname;

  rdm::KnownHosts known(known_hosts_path);
  const std::string host_for_prompt = c.host;
  auto changed = std::make_shared<bool>(false);
  c.trust_server = rdm::TrustWithKnownHosts(
      &known, rdm::KnownHosts::Key(c.host, c.port),
      [&](const std::string& fp) {
        std::fprintf(stderr, "First connection to %s. Server fingerprint:\n  %s\n", host_for_prompt.c_str(),
                     rdm::FormatFingerprint(fp).c_str());
        if (accept_new) return true;
        std::fprintf(stderr, "Compare it with `rdmachine-server --fingerprint` on that machine. Trust it? [y/N] ");
        std::string answer;
        std::getline(std::cin, answer);
        return answer == "y" || answer == "Y" || answer == "yes";
      },
      changed);

  Stats stats;
  stats.keep_last = !save_frame.empty();
  rda_viewer_config_default(&opts.viewer);
  opts.viewer.user = &stats;
  opts.viewer.on_stream_info = &OnStreamInfo;
  opts.viewer.on_frame = &OnFrame;
  opts.clipboard = rdm::SystemClipboard();
  opts.clipboard_fetch_on_announce = true;  // no run loop here to answer at paste time
  opts.log = [](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); };
  opts.on_ended = [&](const std::string& reason) {
    std::fprintf(stderr, "session ended: %s\n", reason.c_str());
    stats.ended = true;
  };

  const std::string host = c.host;  // opts is moved into the session below
  const bool wanted_desktop = c.request.desktop.has_value();
  rdm::ConnectResult result;
  auto session = rdm::RemoteSession::Start(std::move(opts), &result);
  if (!session) {
    if (*changed) {
      std::fprintf(stderr,
                   "THE SERVER'S IDENTITY HAS CHANGED. Refusing to connect.\n"
                   "If you know why (reinstalled server), remove its line from %s.\n",
                   known_hosts_path.c_str());
    } else {
      std::fprintf(stderr, "connection failed: %s%s%s\n", result.error.c_str(), result.detail.empty() ? "" : ": ",
                   result.detail.c_str());
      if (result.retry_after_ms) std::fprintf(stderr, "retry in %lld ms\n", static_cast<long long>(*result.retry_after_ms));
    }
    return 1;
  }
  std::printf("connected to %s (%s), media over %s port %u\n", result.server.server_name.c_str(), host.c_str(),
              session->session().transport == rdm::MediaTransport::kTcp ? "tcp" : "udp", session->session().port);
  const rdm::SessionReady ready = session->session();
  for (const auto& d : ready.displays)
    std::printf("  display %d: %s %dx%d\n", d.index, d.name.c_str(), d.width, d.height);
  if (session->session().desktop) {
    std::printf("  desktop built: %s\n", rdm::FormatDesktopSpec(*session->session().desktop).c_str());
  } else if (wanted_desktop) {
    std::printf("  no desktop built: the server shares its display as it is\n");
  }
  std::fflush(stdout);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (stats.frames < want_frames && !stats.ended && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::printf("received %d frame(s) of %dx%d\n", stats.frames.load(), stats.width.load(), stats.height.load());
  if (set_text_mode && stats.frames > 0) {
    const std::string payload = rdm::EncodePictureMode({*set_text_mode});
    const rda_status st = session->SendMessage(rdm::kPictureModeMessageType,
                                                  reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    std::printf("asked for text mode %s: %s\n", *set_text_mode ? "on" : "off", st == RDA_OK ? "sent" : rda_last_error());
    std::fflush(stdout);
  }
  bool resize_failed = false;
  if (resize_desktop && stats.frames > 0) {
    const std::optional<rdm::DesktopSpec> planned = rdm::PlanDesktop(*resize_desktop);
    const std::string payload = rdm::EncodeDesktopSpec(*resize_desktop);
    const rda_status st = session->SendMessage(rdm::kDesktopRequestMessageType,
                                                  reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    std::printf("asked for desktop %s: %s\n", rdm::FormatDesktopSpec(*resize_desktop).c_str(),
                st == RDA_OK ? "sent" : rda_last_error());
    std::fflush(stdout);
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    auto resized = [&] { return planned && stats.width == planned->width && stats.height == planned->height; };
    const int frames_before = stats.frames;
    while (!(resized() && stats.frames > frames_before) && !stats.ended && std::chrono::steady_clock::now() < limit)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    resize_failed = !resized();
    std::printf("the picture is now %dx%d%s\n", stats.width.load(), stats.height.load(),
                resize_failed ? " (not what was asked for)" : "");
    std::fflush(stdout);
  }
  if (!input_source_json.empty()) {
    const rda_status st = session->SendMessage(rdm::kInputSourceMessageType,
                                                  reinterpret_cast<const uint8_t*>(input_source_json.data()),
                                                  input_source_json.size());
    std::printf("sent input source: %s\n", st == RDA_OK ? "ok" : rda_last_error());
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (click_x >= 0 && stats.frames > 0) {
    auto send = [&](rda_input_kind kind) {
      rda_input_event ev;
      rda_input_event_default(&ev);
      ev.kind = kind;
      ev.x = click_x;
      ev.y = click_y;
      if (kind != RDA_INPUT_MOUSE_MOVE) ev.code = RDA_BUTTON_LEFT;
      session->SendInput(ev);
    };
    send(RDA_INPUT_MOUSE_MOVE);
    if (!move_only) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      send(RDA_INPUT_MOUSE_DOWN);
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      send(RDA_INPUT_MOUSE_UP);
    }
    std::printf("%s %d,%d\n", move_only ? "moved the pointer to" : "clicked at", click_x, click_y);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  if (!save_frame.empty()) {
    std::lock_guard lock(stats.mu);
    if (stats.last.empty() || !WriteBmp(save_frame, stats.last, stats.last_w, stats.last_h)) {
      std::fprintf(stderr, "could not write %s\n", save_frame.c_str());
    } else {
      std::printf("saved the last picture (%dx%d) to %s\n", stats.last_w, stats.last_h, save_frame.c_str());
    }
  }
  session->Close();
  return stats.frames >= want_frames && !resize_failed ? 0 : 1;
}
