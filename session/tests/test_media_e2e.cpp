// The whole session over loopback: ControlServer + MediaHost capturing a real
// X display, RemoteSession receiving pictures. On Linux it needs an X server
// (run it under xvfb-run); skipped without DISPLAY.
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>
#include <mutex>
#include <memory>

#include "fake_clipboard.h"
#include "rdmachine/control_server.h"
#include "rdmachine/desktop.h"
#include "rdmachine/picture_mode.h"
#include "rdmachine/session/media_host.h"
#include "rdmachine/session_stats.h"
#include "rdmachine/session/remote_session.h"
#include "rdmachine/session/viewer.h"

using namespace rdm;
using namespace std::chrono_literals;

namespace {

bool HaveDisplay() {
#if defined(__APPLE__)
  return std::getenv("RDM_TEST_CAPTURE") != nullptr;  // needs Screen Recording permission
#else
  return std::getenv("DISPLAY") != nullptr;
#endif
}

struct Frames {
  std::atomic<int> count{0};
  std::atomic<int> stats_reports{0};
  std::atomic<int> stats_width{0};
  std::atomic<bool> stats_text_mode{false};
  std::atomic<int> width{0};
  std::atomic<rda_status> disconnect_reason{RDA_OK};
  std::atomic<bool> disconnected{false};
};

void OnStreamInfo(void* u, int32_t w, int32_t) { static_cast<Frames*>(u)->width = w; }
void OnFrame(void* u, const rda_image*, const rda_rect*, size_t, const rda_frame_info*) {
  ++static_cast<Frames*>(u)->count;
}
void OnMessage(void* u, uint32_t type, const uint8_t* payload, size_t n) {
  if (type != rdm::kSessionStatsMessageType) return;
  auto r = rdm::DecodeSessionStats(std::string(reinterpret_cast<const char*>(payload), n));
  if (!r) return;
  static_cast<Frames*>(u)->stats_width = r->width;
  static_cast<Frames*>(u)->stats_text_mode = r->text_mode;
  ++static_cast<Frames*>(u)->stats_reports;
}
void OnState(void* u, rda_state s, rda_status reason) {
  if (s == RDA_STATE_DISCONNECTED) {
    static_cast<Frames*>(u)->disconnect_reason = reason;
    static_cast<Frames*>(u)->disconnected = true;
  }
}

template <class F>
bool WaitFor(F f, std::chrono::milliseconds limit = 15s) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!f()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(20ms);
  }
  return true;
}

class MediaE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HaveDisplay()) GTEST_SKIP() << "no display to capture";
    // Ports per process, so parallel test runs do not collide.
    media_port_ = static_cast<uint16_t>(42000 + (getpid() % 4000) * 2);
    MediaHostOptions mo;
    mo.server_name = "xvfb";
    mo.udp_port = media_port_;
    mo.tcp_port = static_cast<uint16_t>(media_port_ + 1);
    mo.inject_input = false;
    mo.fps = 15;
    mo.check_password = [](const std::string& u, const std::string& p, std::string* error) {
      if (u == "alice" && p == "secret") return true;
      *error = "bad_credentials";
      return false;
    };
    mo.clipboard = server_clipboard_;
    Configure(mo);
    media_ = std::make_unique<MediaHost>(mo);
    std::string error;
    static const TlsIdentity id = *GenerateTlsIdentity("xvfb", &error);
    ControlServerOptions co;
    co.bind_address = "127.0.0.1";
    co.port = 0;
    server_ = std::make_unique<ControlServer>(*media_, id, co);
    ASSERT_TRUE(server_->Start(&error)) << error;
  }

  RemoteSession::Options Options(Frames* frames) {
    RemoteSession::Options o;
    o.control.host = "127.0.0.1";
    o.control.port = server_->port();
    o.control.client_name = "test";
    o.control.auth.username = "alice";
    o.control.auth.password = "secret";
    o.control.trust_server = [](const std::string&) { return true; };
    rda_viewer_config_default(&o.viewer);
    o.viewer.user = frames;
    o.viewer.on_stream_info = &OnStreamInfo;
    o.viewer.on_frame = &OnFrame;
    o.viewer.on_state = &OnState;
    o.viewer.on_message = &OnMessage;
    return o;
  }

  virtual void Configure(MediaHostOptions&) {}

  uint16_t media_port_ = 0;
  std::shared_ptr<FakeClipboard> server_clipboard_ = std::make_shared<FakeClipboard>();
  std::unique_ptr<MediaHost> media_;
  std::unique_ptr<ControlServer> server_;
};

// A desktop that only records what was asked of it: the real display stays
// as it is, so the picture keeps its size (the real one: test_virtual_desktop_mac.mm).
struct FakeDesktops {
  std::mutex mu;
  bool fail = false;
  int built = 0, removed = 0;
  std::vector<DesktopSpec> sizes;  // built at, then each Resize()

  int Built() { std::lock_guard l(mu); return built; }
  int Removed() { std::lock_guard l(mu); return removed; }
  std::vector<DesktopSpec> Sizes() { std::lock_guard l(mu); return sizes; }
};

class FakeDesktop : public VirtualDesktop {
 public:
  FakeDesktop(std::shared_ptr<FakeDesktops> all, const DesktopSpec& d) : all_(std::move(all)), spec_(d) {}
  ~FakeDesktop() override {
    std::lock_guard l(all_->mu);
    ++all_->removed;
  }
  DesktopSpec spec() const override { return spec_; }
  bool Resize(const DesktopSpec& d, std::string*) override {
    std::lock_guard l(all_->mu);
    all_->sizes.push_back(d);
    spec_ = d;
    return true;
  }

 private:
  std::shared_ptr<FakeDesktops> all_;
  DesktopSpec spec_;
};

class MediaE2EDesktop : public MediaE2E {
 protected:
  void Configure(MediaHostOptions& mo) override {
    mo.virtual_desktop = [all = desktops_](const DesktopSpec& d, std::string* error) -> std::unique_ptr<VirtualDesktop> {
      std::lock_guard l(all->mu);
      if (all->fail) {
        *error = "no virtual display today";
        return nullptr;
      }
      ++all->built;
      all->sizes.push_back(d);
      return std::make_unique<FakeDesktop>(all, d);
    };
  }

  static void Ask(RemoteSession& s, const DesktopSpec& d) {
    const std::string payload = EncodeDesktopSpec(d);
    ASSERT_EQ(rda_viewer_send_message(s.viewer(), kDesktopRequestMessageType,
                                      reinterpret_cast<const uint8_t*>(payload.data()), payload.size()),
              RDA_OK);
  }

  std::shared_ptr<FakeDesktops> desktops_ = std::make_shared<FakeDesktops>();
};

}  // namespace

TEST_F(MediaE2EDesktop, BuildsTheDesktopTheClientAsksFor) {
  Frames frames;
  auto o = Options(&frames);
  o.control.request.desktop = DesktopSpec{2879, 1801, 2.0};
  o.control.request.display_index = 7;  // not used with a desktop: it is display 0
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  // Built as planned (whole points), and the client is told what it got.
  ASSERT_TRUE(s->session().desktop);
  EXPECT_EQ(*s->session().desktop, (DesktopSpec{2878, 1800, 2.0}));
  EXPECT_EQ(desktops_->Built(), 1);
  EXPECT_TRUE(WaitFor([&] { return frames.count > 0; }));
  MediaHostStatus st = media_->Status();
  ASSERT_TRUE(st.desktop);
  EXPECT_EQ(*st.desktop, (DesktopSpec{2878, 1800, 2.0}));
  EXPECT_EQ(st.width, 2878);

  // The client's window changed: the desktop is rebuilt and the picture goes on.
  Ask(*s, DesktopSpec{2048, 1536, 2.0});
  EXPECT_TRUE(WaitFor([&] { return desktops_->Sizes().size() == 2; }));
  EXPECT_EQ(desktops_->Sizes().back(), (DesktopSpec{2048, 1536, 2.0}));
  EXPECT_TRUE(WaitFor([&] { return media_->Status().desktop == DesktopSpec{2048, 1536, 2.0}; }));
  const int before = frames.count;
  EXPECT_TRUE(WaitFor([&] { return frames.count > before; }));
  // The overlay's figures follow.
  EXPECT_TRUE(WaitFor([&] { return frames.stats_width == 2048; }));

  // The same size again, and nonsense: nothing to do.
  Ask(*s, DesktopSpec{2049, 1537, 2.0});
  Ask(*s, DesktopSpec{0, 0, 2.0});
  // Another one goes through after them.
  Ask(*s, DesktopSpec{1600, 1000, 1.0});
  EXPECT_TRUE(WaitFor([&] { return desktops_->Sizes().size() == 3; }));
  EXPECT_EQ(desktops_->Sizes().back(), (DesktopSpec{1600, 1000, 1.0}));
  EXPECT_EQ(desktops_->Removed(), 0);

  // The desktop goes with the session.
  s->Close();
  EXPECT_TRUE(WaitFor([&] { return !media_->session_running(); }));
  EXPECT_EQ(desktops_->Removed(), 1);
  EXPECT_FALSE(media_->Status().desktop);
}

TEST_F(MediaE2EDesktop, SharesTheDisplayAsItIsWhenTheDesktopCannotBeBuilt) {
  {
    std::lock_guard l(desktops_->mu);
    desktops_->fail = true;
  }
  Frames frames;
  auto o = Options(&frames);
  o.control.request.desktop = DesktopSpec{2880, 1800, 2.0};
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_FALSE(s->session().desktop);
  EXPECT_TRUE(WaitFor([&] { return frames.count > 0; }));
  EXPECT_EQ(frames.width, s->session().displays[0].width);
  // Asking again later changes nothing: there is no desktop to resize.
  const std::string payload = EncodeDesktopSpec(DesktopSpec{2048, 1536, 2.0});
  rda_viewer_send_message(s->viewer(), kDesktopRequestMessageType, reinterpret_cast<const uint8_t*>(payload.data()),
                          payload.size());
  const int before = frames.stats_reports;
  EXPECT_TRUE(WaitFor([&] { return frames.stats_reports > before; }));
  EXPECT_FALSE(media_->Status().desktop);
  EXPECT_EQ(desktops_->Built(), 0);
}

TEST_F(MediaE2E, ADesktopRequestIsIgnoredByAServerThatBuildsNone) {
  Frames frames;
  auto o = Options(&frames);
  o.control.request.desktop = DesktopSpec{2880, 1800, 2.0};
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_FALSE(s->session().desktop);
  EXPECT_TRUE(WaitFor([&] { return frames.count > 0; }));
}

TEST_F(MediaE2E, PictureArrivesOverUdp) {
  Frames frames;
  ConnectResult r;
  auto s = RemoteSession::Start(Options(&frames), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_EQ(s->session().transport, MediaTransport::kUdp);
  EXPECT_EQ(s->session().port, media_port_);
  ASSERT_FALSE(s->session().displays.empty());
  EXPECT_TRUE(WaitFor([&] { return frames.count > 0; }));
  EXPECT_EQ(frames.width, s->session().displays[0].width);
  EXPECT_TRUE(media_->session_running());

  // What the menu bar app shows.
  MediaHostStatus st = media_->Status();
  EXPECT_TRUE(st.running);
  EXPECT_TRUE(st.media_connected);
  EXPECT_EQ(st.origin.username, "alice");
  EXPECT_EQ(st.origin.peer_ip, "127.0.0.1");
  EXPECT_EQ(st.transport, MediaTransport::kUdp);
  EXPECT_TRUE(st.view_only);  // inject_input = false
  EXPECT_FALSE(st.input_unavailable);  // view only by choice, not for want of permission
  EXPECT_EQ(st.width, s->session().displays[0].width);
  EXPECT_GT(st.stats.frames_encoded, 0u);
  EXPECT_GT(st.stats.packets_sent, 0u);  // UDP measures the path (rdaccel 1.1)
  EXPECT_FALSE(st.encoder.empty());

  // The overlay's figures arrive about once a second.
  EXPECT_TRUE(WaitFor([&] { return frames.stats_reports > 0; }));
  EXPECT_EQ(frames.stats_width, s->session().displays[0].width);

  s->Close();
  EXPECT_TRUE(WaitFor([&] { return !media_->session_running(); }));
  EXPECT_FALSE(media_->Status().running);
}

TEST_F(MediaE2E, TextModeIsAskedForAtTheStartAndChangedWhileRunning) {
  // docs/protocol.md, "文字模式": session_request.text_mode, then the picture
  // mode message. The picture keeps coming in either mode.
  Frames frames;
  auto o = Options(&frames);
  o.control.request.text_mode = true;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_TRUE(WaitFor([&] { return frames.count > 0; }));
  EXPECT_TRUE(media_->Status().text_mode);
  EXPECT_TRUE(WaitFor([&] { return frames.stats_text_mode.load(); }));  // the overlay is told

  auto send = [&](bool on) {
    const std::string payload = EncodePictureMode({on});
    return rda_viewer_send_message(s->viewer(), kPictureModeMessageType,
                                   reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  };
  ASSERT_EQ(send(false), RDA_OK);
  EXPECT_TRUE(WaitFor([&] { return !media_->Status().text_mode; }));
  // Leaving text mode refreshes the whole picture.
  const int before = frames.count;
  EXPECT_TRUE(WaitFor([&] { return frames.count > before; }));
  EXPECT_TRUE(WaitFor([&] { return !frames.stats_text_mode.load(); }));
  ASSERT_EQ(send(true), RDA_OK);
  EXPECT_TRUE(WaitFor([&] { return media_->Status().text_mode; }));
  s->Close();
  EXPECT_TRUE(WaitFor([&] { return !media_->session_running(); }));
  EXPECT_FALSE(media_->Status().text_mode);
}

TEST_F(MediaE2E, PictureArrivesOverTcp) {
  Frames frames;
  auto o = Options(&frames);
  o.control.request.transport = MediaTransport::kTcp;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_EQ(s->session().transport, MediaTransport::kTcp);
  EXPECT_TRUE(WaitFor([&] { return frames.count > 0; }));
}

TEST_F(MediaE2E, ViewerWithoutTheTokenIsRefused) {
  // Log in for a session, then connect to rdaccel with the right key but a
  // wrong token: the host must refuse it.
  ClientOptions c = Options(nullptr).control;
  ConnectResult r = ConnectAndStartSession(c);
  ASSERT_EQ(r.error, "") << r.detail;
  SessionReady forged = r.session;
  forged.auth_token[0] ^= 1;
  Frames frames;
  rda_viewer_config vc;
  rda_viewer_config_default(&vc);
  vc.user = &frames;
  vc.on_frame = &OnFrame;
  vc.on_state = &OnState;
  rda_viewer* v = nullptr;
  std::string error;
  ASSERT_EQ(CreateSessionViewer("127.0.0.1", forged, vc, &v, &error), RDA_OK) << error;
  EXPECT_TRUE(WaitFor([&] { return frames.disconnected.load(); }));
  EXPECT_EQ(frames.disconnect_reason, RDA_ERR_AUTH);
  EXPECT_EQ(frames.count, 0);
  rda_viewer_destroy(v);
}

TEST_F(MediaE2E, SecondClientIsBusyWhileSessionRuns) {
  Frames frames;
  ConnectResult r;
  auto s = RemoteSession::Start(Options(&frames), &r);
  ASSERT_TRUE(s) << r.error;
  ConnectResult second = ConnectAndStartSession(Options(nullptr).control);
  EXPECT_EQ(second.error, "busy");
}

TEST_F(MediaE2E, ServerStopEndsClientSession) {
  Frames frames;
  std::atomic<bool> ended{false};
  std::string reason;
  auto o = Options(&frames);
  o.on_ended = [&](const std::string& why) {
    reason = why;
    ended = true;
  };
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error;
  server_->Stop();
  EXPECT_TRUE(WaitFor([&] { return ended.load(); }));
  EXPECT_EQ(reason, "server_shutdown");
  EXPECT_FALSE(media_->session_running());
}

namespace {

// Sessions that may control the server (so the clipboard goes both ways);
// XTest on the Xvfb screen.
class ClipboardE2E : public MediaE2E {
 protected:
  void Configure(MediaHostOptions& mo) override { mo.inject_input = true; }
};

}  // namespace

TEST_F(ClipboardE2E, TextGoesBothWays) {
  Frames frames;
  auto client_clipboard = std::make_shared<FakeClipboard>();
  client_clipboard->Copy("copied before connecting");
  auto o = Options(&frames);
  o.clipboard = client_clipboard;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;

  // What the client had is pasted on the server.
  EXPECT_TRUE(WaitFor([&] { return server_clipboard_->text() == "copied before connecting"; }));
  // Server to client, including non-ASCII text.
  server_clipboard_->Copy("服务端的文字 ✓");
  EXPECT_TRUE(WaitFor([&] { return client_clipboard->text() == "服务端的文字 ✓"; }));
  // Client to server again.
  client_clipboard->Copy("client again");
  EXPECT_TRUE(WaitFor([&] { return server_clipboard_->text() == "client again"; }));
  // No echo: each side wrote exactly what it received.
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(server_clipboard_->writes(), 2);
  EXPECT_EQ(client_clipboard->writes(), 1);
  s->Close();
}

TEST_F(MediaE2E, ViewOnlySessionDoesNotTakeTheClientsClipboard) {
  // The fixture's server does not inject input, so every session is view
  // only: the server's clipboard still reaches the client.
  Frames frames;
  auto client_clipboard = std::make_shared<FakeClipboard>();
  client_clipboard->Copy("from the client");
  auto o = Options(&frames);
  o.clipboard = client_clipboard;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  server_clipboard_->Copy("from the server");
  EXPECT_TRUE(WaitFor([&] { return client_clipboard->text() == "from the server"; }));
  client_clipboard->Copy("from the client, later");
  std::this_thread::sleep_for(700ms);
  EXPECT_EQ(server_clipboard_->text(), "from the server");
  EXPECT_EQ(server_clipboard_->writes(), 0);
}
