// The whole session over loopback: ControlServer + MediaHost capturing a real
// X display, RemoteSession receiving pictures. On Linux it needs an X server
// (run it under xvfb-run); skipped without DISPLAY.
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "fake_clipboard.h"
#include "rdmachine/control_server.h"
#include "rdmachine/session/media_host.h"
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
  std::atomic<int> width{0};
  std::atomic<rda_status> disconnect_reason{RDA_OK};
  std::atomic<bool> disconnected{false};
};

void OnStreamInfo(void* u, int32_t w, int32_t) { static_cast<Frames*>(u)->width = w; }
void OnFrame(void* u, const rda_image*, const rda_rect*, size_t, const rda_frame_info*) {
  ++static_cast<Frames*>(u)->count;
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
    return o;
  }

  virtual void Configure(MediaHostOptions&) {}

  uint16_t media_port_ = 0;
  std::shared_ptr<FakeClipboard> server_clipboard_ = std::make_shared<FakeClipboard>();
  std::unique_ptr<MediaHost> media_;
  std::unique_ptr<ControlServer> server_;
};

}  // namespace

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
  EXPECT_EQ(st.width, s->session().displays[0].width);
  EXPECT_GT(st.stats.frames_encoded, 0u);
  EXPECT_GT(st.stats.packets_sent, 0u);  // UDP measures the path (rdaccel 1.1)
  EXPECT_FALSE(st.encoder.empty());

  s->Close();
  EXPECT_TRUE(WaitFor([&] { return !media_->session_running(); }));
  EXPECT_FALSE(media_->Status().running);
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
