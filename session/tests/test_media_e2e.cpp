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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <memory>

#include "fake_clipboard.h"
#include "rdmachine/control_server.h"
#include "rdmachine/desktop.h"
#include "rdmachine/audio_control.h"
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
  std::mutex audio_mu;
  std::string stats_audio;  // the last report's "audio" (under audio_mu)
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
  {
    std::lock_guard lock(static_cast<Frames*>(u)->audio_mu);
    static_cast<Frames*>(u)->stats_audio = r->audio;
  }
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
    // Sound, if a test asks for it, is rdaccel's computed test signal: no
    // test captures this Mac's sound, let alone mutes it.
    mo.audio_source = RDA_AUDIO_SOURCE_TEST;
    mo.mute_local_audio = false;
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

// A server in the login window session (inject_input stays off, so nothing
// is ever typed into the real screen here).
class MediaE2ELoginWindow : public MediaE2E {
 protected:
  void Configure(MediaHostOptions& mo) override {
    mo.login_window = true;
    mo.login_screen = [] { return LoginScreenState{true, true}; };
  }
};

}  // namespace

TEST_F(MediaE2ELoginWindow, TheSessionSaysItIsAtTheLoginWindow) {
  Frames frames;
  ConnectResult r;
  auto s = RemoteSession::Start(Options(&frames), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_TRUE(s->session().login_window);
  EXPECT_TRUE(WaitFor([&] { return frames.count > 0; }));
}

TEST_F(MediaE2E, AnOrdinarySessionIsNotAtTheLoginWindow) {
  Frames frames;
  ConnectResult r;
  auto s = RemoteSession::Start(Options(&frames), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_FALSE(s->session().login_window);
}

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

TEST_F(MediaE2E, ASecondClientTakesTheSessionOver) {
  Frames frames;
  ConnectResult r;
  auto o = Options(&frames);
  std::mutex mu;
  std::string ended;
  o.on_ended = [&](const std::string& reason) {
    std::lock_guard l(mu);
    ended = reason;
  };
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error;
  Frames frames2;
  ConnectResult r2;
  auto s2 = RemoteSession::Start(Options(&frames2), &r2);
  ASSERT_TRUE(s2) << r2.error << ": " << r2.detail;
  EXPECT_TRUE(WaitFor([&] { return frames2.count > 0; }));
  EXPECT_TRUE(WaitFor([&] {
    std::lock_guard l(mu);
    return ended == "session_replaced";
  }));
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

// Where the client's copied files appear on the server: records what it is told.
struct RecordingFileTarget : FileTarget {
  void SetRequester(FileRequester* r) override {
    std::lock_guard lock(mu);
    requester = r;
  }
  void DropRequester(FileRequester* r) override {
    std::lock_guard lock(mu);
    if (requester == r) requester = nullptr;
  }
  std::optional<std::vector<std::string>> Publish(uint64_t announce, const std::vector<FileEntry>& items) override {
    std::lock_guard lock(mu);
    published = announce;
    std::vector<std::string> out;
    for (const auto& e : items) out.push_back("/placeholders/" + e.path);
    return out;
  }
  void Withdraw(uint64_t) override {}
  void OnList(uint64_t, uint32_t, const std::vector<FileEntry>&, const std::string&) override {}
  void OnProgress(uint64_t, uint32_t, const std::string&, uint64_t) override {}
  void OnFileReady(uint64_t, uint32_t, const FileEntry&, const std::string&) override {
    std::lock_guard lock(mu);
    ++ready;
  }
  void OnItemDone(uint64_t, uint32_t, const std::string& error) override {
    std::lock_guard lock(mu);
    done.push_back(error);
  }
  std::mutex mu;
  FileRequester* requester = nullptr;
  uint64_t published = 0;
  int ready = 0;
  std::vector<std::string> done;
};

class FilesE2E : public MediaE2E {
 protected:
  void Configure(MediaHostOptions& mo) override {
    mo.inject_input = true;
    char tmpl[] = "/tmp/rdm-e2e-files-XXXXXX";
    root_ = mkdtemp(tmpl);
    mo.file_target = target_;
    mo.file_staging_dir = root_ + "/staging";
  }
  void TearDown() override {
    MediaE2E::TearDown();
    if (!root_.empty()) std::system(("rm -rf '" + root_ + "'").c_str());
  }
  std::shared_ptr<RecordingFileTarget> target_ = std::make_shared<RecordingFileTarget>();
  std::string root_;
};



}  // namespace

TEST_F(ClipboardE2E, TextIsFetchedWhenPasted) {
  Frames frames;
  auto client_clipboard = std::make_shared<FakeClipboard>();
  client_clipboard->Copy("copied before connecting");
  auto o = Options(&frames);
  o.clipboard = client_clipboard;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;

  // What the client had before connecting is not offered.
  std::this_thread::sleep_for(700ms);
  EXPECT_FALSE(server_clipboard_->promise());
  // A copy on the client is promised on the server, and fetched when pasted.
  client_clipboard->Copy("客户端的文字 ✓");
  ASSERT_TRUE(WaitFor([&] { return server_clipboard_->promise() != nullptr; }));
  EXPECT_EQ(server_clipboard_->promise()->FetchFormat(std::string(kMimeText), nullptr), "客户端的文字 ✓");
  // Server to client.
  server_clipboard_->Copy("服务端的文字 ✓");
  ASSERT_TRUE(WaitFor([&] { return client_clipboard->promise() != nullptr; }));
  EXPECT_EQ(client_clipboard->promise()->FetchFormat(std::string(kMimeText), nullptr), "服务端的文字 ✓");
  // No echo: neither side wrote anything but the one promise.
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(server_clipboard_->writes(), 1);
  EXPECT_EQ(client_clipboard->writes(), 1);
  s->Close();
}

TEST_F(FilesE2E, AFolderCopiedOnTheClientIsFetchedWhenPasted) {
  // folder/{big.bin (3 MiB), sub/small.txt}
  const std::string folder = root_ + "/folder";
  ASSERT_EQ(system(("mkdir -p '" + folder + "/sub'").c_str()), 0);
  std::string big(3u << 20, '\0');
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 7 + i / 1001) & 0xFF);
  { std::ofstream(folder + "/big.bin", std::ios::binary) << big; }
  { std::ofstream(folder + "/sub/small.txt") << "小文件"; }

  Frames frames;
  auto client_clipboard = std::make_shared<FakeClipboard>();
  auto o = Options(&frames);
  o.clipboard = client_clipboard;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  std::this_thread::sleep_for(700ms);

  ClipboardContent c = ClipboardContent::Text("folder");
  c.files = {folder};
  client_clipboard->CopyContent(c);
  ASSERT_TRUE(WaitFor([&] { return server_clipboard_->promise() != nullptr; }));
  EXPECT_EQ(server_clipboard_->promise()->file_paths(), std::vector<std::string>{"/placeholders/folder"});

  uint64_t announce = 0;
  {
    std::lock_guard lock(target_->mu);
    ASSERT_TRUE(target_->requester);
    announce = target_->published;
    target_->requester->RequestFileFetch(announce, 0, "folder");
  }
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard lock(target_->mu);
    return !target_->done.empty();
  }, 20s));
  {
    std::lock_guard lock(target_->mu);
    EXPECT_EQ(target_->done, std::vector<std::string>{""});
    EXPECT_EQ(target_->ready, 4);  // the folder, sub, and two files
  }
  const std::string got = root_ + "/staging/" + std::to_string(announce) + "/0/folder";
  std::ifstream f(got + "/big.bin", std::ios::binary);
  std::string read((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  EXPECT_TRUE(read == big);
  std::ifstream g(got + "/sub/small.txt");
  std::string small((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>());
  EXPECT_EQ(small, "小文件");
  s->Close();
}

TEST_F(MediaE2E, ViewOnlySessionDoesNotTakeTheClientsClipboard) {
  // The fixture's server does not inject input, so every session is view
  // only: the server's clipboard still reaches the client.
  Frames frames;
  auto client_clipboard = std::make_shared<FakeClipboard>();
  auto o = Options(&frames);
  o.clipboard = client_clipboard;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  server_clipboard_->Copy("from the server");
  ASSERT_TRUE(WaitFor([&] { return client_clipboard->promise() != nullptr; }));
  EXPECT_EQ(client_clipboard->promise()->FetchFormat(std::string(kMimeText), nullptr), "from the server");
  client_clipboard->Copy("from the client");
  std::this_thread::sleep_for(700ms);
  EXPECT_FALSE(server_clipboard_->promise());
  EXPECT_EQ(server_clipboard_->writes(), 0);
}

// ---- sound (docs/audio.md) ------------------------------------------------

namespace {

// The server's sound is rdaccel's computed test signal: no device is
// touched on either side (the client plays on the null device).
class MediaE2EAudio : public MediaE2E {
 protected:
  void Configure(MediaHostOptions& mo) override {
    mo.audio_source = RDA_AUDIO_SOURCE_TEST;
    mo.mute_local_audio = false;
  }
};

class MediaE2EAudioLoginWindow : public MediaE2E {
 protected:
  void Configure(MediaHostOptions& mo) override {
    mo.audio_source = RDA_AUDIO_SOURCE_TEST;
    mo.login_window = true;
    mo.login_screen = [] { return LoginScreenState{true, true}; };
  }
};

rda_viewer_stats ViewerStats(rda_viewer* v) {
  rda_viewer_stats s{};
  s.struct_size = sizeof(s);
  if (!v || rda_viewer_get_stats(v, &s) != RDA_OK) s = rda_viewer_stats{};
  return s;
}

std::string StatsAudio(Frames& f) {
  std::lock_guard lock(f.audio_mu);
  return f.stats_audio;
}

}  // namespace

TEST_F(MediaE2EAudio, SoundReachesTheClient) {
  Frames frames;
  auto o = Options(&frames);
  o.audio = true;
  o.audio_null_device = true;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_TRUE(s->session().audio);
  EXPECT_TRUE(s->audio_playing());
  EXPECT_TRUE(WaitFor([&] { return ViewerStats(s->viewer()).audio_packets > 0; }, 5s));
  EXPECT_TRUE(WaitFor([&] {
    const std::string a = StatsAudio(frames);
    return a == "on" || a == "silent";
  }));
  const MediaHostStatus st = media_->Status();
  EXPECT_TRUE(st.audio);
  EXPECT_EQ(st.audio_status.frame_ms, 10);
  EXPECT_GT(st.stats.audio_packets, 0u);
  EXPECT_GE(st.stats.bytes_sent, st.stats.audio_bytes);
  s->Close();
  EXPECT_TRUE(WaitFor([&] { return !media_->session_running(); }));
  EXPECT_FALSE(media_->Status().audio);
}

TEST_F(MediaE2EAudio, NoSoundForAClientThatDidNotAsk) {
  Frames frames;
  ConnectResult r;
  auto s = RemoteSession::Start(Options(&frames), &r);  // audio = false
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_FALSE(s->session().audio);
  EXPECT_EQ(s->session().audio_reason, "");  // it did not ask: no reason to give
  EXPECT_FALSE(s->audio_playing());
  EXPECT_TRUE(WaitFor([&] { return frames.stats_reports > 0; }));
  std::this_thread::sleep_for(1500ms);
  EXPECT_EQ(ViewerStats(s->viewer()).audio_packets, 0u);
  const MediaHostStatus st = media_->Status();
  EXPECT_FALSE(st.audio);
  EXPECT_EQ(st.audio_reason, "client");
  EXPECT_EQ(st.stats.audio_packets, 0u);
  EXPECT_EQ(StatsAudio(frames), "off");
}

TEST_F(MediaE2EAudio, PausedSendsNothing) {
  Frames frames;
  auto o = Options(&frames);
  o.audio = true;
  o.audio_null_device = true;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  ASSERT_TRUE(WaitFor([&] { return ViewerStats(s->viewer()).audio_packets > 0; }, 5s));
  s->SetAudioPaused(true);
  EXPECT_TRUE(WaitFor([&] { return media_->Status().audio_status.state == RDA_AUDIO_STATE_PAUSED; }));
  std::this_thread::sleep_for(300ms);  // what was on its way
  const uint64_t before = media_->Status().stats.audio_packets;
  std::this_thread::sleep_for(4500ms);  // longer than the test signal's loop: sound would have come
  EXPECT_EQ(media_->Status().stats.audio_packets, before);
  EXPECT_TRUE(WaitFor([&] { return StatsAudio(frames) == "paused"; }));
  s->SetAudioPaused(false);
  EXPECT_TRUE(WaitFor([&] { return media_->Status().stats.audio_packets > before; }, 6s));
}

TEST_F(MediaE2EAudio, DataSaverSwitchesTheProfile) {
  Frames frames;
  auto o = Options(&frames);
  o.audio = true;
  o.audio_null_device = true;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  ASSERT_TRUE(WaitFor([&] { return ViewerStats(s->viewer()).audio_packets > 0; }, 5s));
  EXPECT_EQ(media_->Status().audio_status.frame_ms, 10);
  EXPECT_EQ(media_->Status().audio_status.profile, RDA_AUDIO_PROFILE_FULL);
  const std::string payload = EncodePictureMode({true});
  ASSERT_EQ(s->SendMessage(kPictureModeMessageType, reinterpret_cast<const uint8_t*>(payload.data()), payload.size()),
            RDA_OK);
  // From the next frame (the sound may be in a silent stretch: the profile
  // shows at once, the frame length with the next sound).
  EXPECT_TRUE(WaitFor([&] { return media_->Status().audio_status.profile == RDA_AUDIO_PROFILE_SAVER; }));
  EXPECT_TRUE(WaitFor([&] { return media_->Status().audio_status.frame_ms == 20; }, 6s));
}

TEST_F(MediaE2EAudioLoginWindow, NoSoundAtTheLoginWindow) {
  Frames frames;
  auto o = Options(&frames);
  o.audio = true;
  o.audio_null_device = true;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_TRUE(s->session().login_window);
  EXPECT_FALSE(s->session().audio);
  EXPECT_EQ(s->session().audio_reason, "login_window");
  EXPECT_FALSE(s->audio_playing());
  EXPECT_FALSE(media_->Status().audio);
}

TEST_F(MediaE2EAudio, AnOlderClientGetsNoSound) {
  // A client whose rdaccel takes no sound (1.12 and older): it may put
  // session_request.audio, but its viewer has no kCapAudio, so the server
  // captures nothing and sends nothing.
  ClientOptions c;
  c.host = "127.0.0.1";
  c.port = server_->port();
  c.client_name = "older";
  c.auth.username = "alice";
  c.auth.password = "secret";
  c.trust_server = [](const std::string&) { return true; };
  c.request.audio = true;
  ConnectResult r = ConnectAndStartSession(c);
  ASSERT_TRUE(r.error.empty()) << r.error << ": " << r.detail;
  EXPECT_TRUE(r.session.audio);
  Frames frames;
  rda_viewer_config vc;
  rda_viewer_config_default(&vc);
  vc.user = &frames;
  vc.on_frame = &OnFrame;
  vc.audio = 0;  // as an older viewer: no sound in its handshake
  rda_viewer* v = nullptr;
  std::string error;
  ASSERT_EQ(CreateSessionViewer("127.0.0.1", r.session, vc, &v, &error), RDA_OK) << error;
  EXPECT_TRUE(WaitFor([&] { return frames.count > 0; }));
  std::this_thread::sleep_for(1500ms);
  const MediaHostStatus st = media_->Status();
  EXPECT_EQ(st.audio_status.state, RDA_AUDIO_STATE_WAITING_PEER);
  EXPECT_EQ(st.stats.audio_packets, 0u);
  rda_viewer_destroy(v);
  r.connection->Close("user_closed");
}

TEST_F(MediaE2EAudio, AMutedOutputHereSendsNothing) {
  // docs/audio.md, 5.6: this Mac's output muted (as the player reads it,
  // stood in for here): the sound is paused, and goes on once it is audible.
  std::atomic<int> muted{0};
  Frames frames;
  auto o = Options(&frames);
  o.audio = true;
  o.audio_null_device = true;
  o.output_muted = [&muted] { return muted.load(); };
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  ASSERT_TRUE(WaitFor([&] { return ViewerStats(s->viewer()).audio_packets > 0; }, 5s));
  EXPECT_FALSE(s->audio_paused());
  muted = 1;
  EXPECT_TRUE(WaitFor([&] { return media_->Status().audio_status.state == RDA_AUDIO_STATE_PAUSED; }, 5s));
  EXPECT_TRUE(s->audio_paused());
  muted = -1;  // not known: as audible
  EXPECT_TRUE(WaitFor([&] { return media_->Status().audio_status.state != RDA_AUDIO_STATE_PAUSED; }, 5s));
  EXPECT_FALSE(s->audio_paused());
  // The menu's pause and the muted output are two reasons: either keeps it paused.
  s->SetAudioPaused(true);
  muted = 1;
  std::this_thread::sleep_for(1500ms);
  muted = 0;
  std::this_thread::sleep_for(1500ms);
  EXPECT_EQ(media_->Status().audio_status.state, RDA_AUDIO_STATE_PAUSED);
  s->SetAudioPaused(false);
  EXPECT_TRUE(WaitFor([&] { return media_->Status().audio_status.state != RDA_AUDIO_STATE_PAUSED; }, 5s));
}

TEST_F(MediaE2EAudio, NoSoundOverTcp) {
  // The picture over TCP carries no sound: the session says so, rdaccel's
  // sound is not started, and the client does not wait for it.
  Frames frames;
  auto o = Options(&frames);
  o.audio = true;
  o.audio_null_device = true;
  o.control.request.transport = MediaTransport::kTcp;
  ConnectResult r;
  auto s = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(s) << r.error << ": " << r.detail;
  EXPECT_EQ(s->session().transport, MediaTransport::kTcp);
  EXPECT_FALSE(s->session().audio);
  EXPECT_EQ(s->session().audio_reason, "tcp");
  EXPECT_FALSE(s->audio_playing());
  EXPECT_TRUE(WaitFor([&] { return frames.stats_reports > 0; }));
  const MediaHostStatus st = media_->Status();
  EXPECT_FALSE(st.audio);
  EXPECT_EQ(st.audio_reason, "tcp");
  EXPECT_EQ(st.audio_status.state, RDA_AUDIO_STATE_OFF);
  EXPECT_EQ(StatsAudio(frames), "off");
  EXPECT_EQ(s->revision(), kProtocolRevision);
}
