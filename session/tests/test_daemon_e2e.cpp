// The daemon and its agents over loopback: ControlServer + Daemon, a login
// window agent and a user agent (each a MediaHost capturing the real screen,
// input off), and a RemoteSession following its session from the login
// window to the user's session when the console changes hands. Needs screen
// capture (RDM_TEST_CAPTURE=1 on macOS, DISPLAY on Linux).
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "rdmachine/control_server.h"
#include "rdmachine/session/agent_host.h"
#include "rdmachine/session/daemon.h"
#include "rdmachine/session/media_host.h"
#include "rdmachine/session/remote_session.h"

using namespace rdm;
using namespace std::chrono_literals;

namespace {

bool HaveDisplay() {
#if defined(__APPLE__)
  return std::getenv("RDM_TEST_CAPTURE") != nullptr;
#else
  return std::getenv("DISPLAY") != nullptr;
#endif
}

template <class F>
bool WaitFor(F f, std::chrono::milliseconds limit = 15s) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (f()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return f();
}

struct Seen {
  std::atomic<int> frames{0};
  std::atomic<int> moves{0};
  std::atomic<bool> moved_to_login_window{true};
  std::mutex mu;
  std::string ended;
  std::string Ended() {
    std::lock_guard l(mu);
    return ended;
  }
};

void OnFrame(void* u, const rda_image*, const rda_rect*, size_t, const rda_frame_info*) {
  ++static_cast<Seen*>(u)->frames;
}

class DaemonE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HaveDisplay()) GTEST_SKIP() << "no display to capture";
    socket_ = "/tmp/rdm-test-" + std::to_string(getpid()) + ".sock";
    media_port_ = static_cast<uint16_t>(46000 + (getpid() % 4000) * 2);

    DaemonOptions d;
    d.server_name = "test";
    d.socket_path = socket_;
    d.console = [this] {
      std::lock_guard l(console_mu_);
      return console_;
    };
    d.check_password = [](const std::string& u, const std::string& p, std::string* error) {
      if ((u == "alice" || u == "bob") && p == "secret") return true;
      *error = "bad_credentials";
      return false;
    };
    d.trust_agent = [](int, std::string* why) {
      why->clear();
      return true;
    };
    d.check_agent_uid = false;
    daemon_ = std::make_unique<Daemon>(d);
    std::string error;
    ASSERT_TRUE(daemon_->Start(&error)) << error;

    static const TlsIdentity id = *GenerateTlsIdentity("test", &error);
    ControlServerOptions co;
    co.bind_address = "127.0.0.1";
    co.port = 0;
    server_ = std::make_unique<ControlServer>(*daemon_, id, co);
    ASSERT_TRUE(server_->Start(&error)) << error;

    login_media_ = std::make_unique<MediaHost>(Media(true));
    user_media_ = std::make_unique<MediaHost>(Media(false));
    login_agent_ = std::make_unique<AgentHost>(*login_media_, AgentHostOptions{socket_, "loginwindow", {}});
    user_agent_ = std::make_unique<AgentHost>(*user_media_, AgentHostOptions{socket_, "aqua", {}});
    login_agent_->Start();
    user_agent_->Start();
    ASSERT_TRUE(WaitFor([&] { return daemon_->agent_count() == 2; }));
  }

  void TearDown() override {
    if (session_) session_->Close();
    session_.reset();
    if (login_agent_) login_agent_->Stop();
    if (user_agent_) user_agent_->Stop();
    if (server_) server_->Stop();
    if (daemon_) daemon_->Stop();
  }

  MediaHostOptions Media(bool login_window) {
    MediaHostOptions mo;
    mo.server_name = "test";
    mo.udp_port = media_port_;  // both agents use the same ports, one at a time
    mo.tcp_port = static_cast<uint16_t>(media_port_ + 1);
    mo.inject_input = false;  // nothing is ever typed into the real screen
    mo.fps = 15;
    mo.login_window = login_window;
    return mo;
  }

  void SetConsole(uint32_t uid, std::string user) {
    std::lock_guard l(console_mu_);
    console_ = ConsoleOwner{uid, std::move(user)};
  }

  RemoteSession::Options Options(const std::string& user) {
    RemoteSession::Options o;
    o.control.host = "127.0.0.1";
    o.control.port = server_->port();
    o.control.client_name = "test";
    o.control.auth.username = user;
    o.control.auth.password = "secret";
    o.control.trust_server = [](const std::string&) { return true; };
    rda_viewer_config_default(&o.viewer);
    o.viewer.user = &seen_;
    o.viewer.on_frame = &OnFrame;
    o.on_moved = [this](const SessionReady& r) {
      seen_.moved_to_login_window = r.login_window;
      ++seen_.moves;
    };
    o.on_ended = [this](const std::string& reason) {
      std::lock_guard l(seen_.mu);
      seen_.ended = reason;
    };
    return o;
  }

  std::string socket_;
  uint16_t media_port_ = 0;
  std::mutex console_mu_;
  ConsoleOwner console_{0, ""};  // the login window
  std::unique_ptr<Daemon> daemon_;
  std::unique_ptr<ControlServer> server_;
  std::unique_ptr<MediaHost> login_media_, user_media_;
  std::unique_ptr<AgentHost> login_agent_, user_agent_;
  std::unique_ptr<RemoteSession> session_;
  Seen seen_;
};

}  // namespace

TEST_F(DaemonE2E, TheSessionFollowsTheUserFromTheLoginWindowToTheirDesktop) {
  ConnectResult r;
  session_ = RemoteSession::Start(Options("alice"), &r);
  ASSERT_TRUE(session_) << r.error << ": " << r.detail;
  EXPECT_TRUE(session_->session().login_window);
  EXPECT_TRUE(login_media_->session_running());
  EXPECT_TRUE(WaitFor([&] { return seen_.frames > 0; }));

  // alice logs in at the login window: the console is hers now.
  SetConsole(getuid(), "alice");
  ASSERT_TRUE(WaitFor([&] { return seen_.moves.load() == 1; }, 30s)) << "ended: " << seen_.Ended();
  EXPECT_FALSE(seen_.moved_to_login_window);
  EXPECT_FALSE(session_->session().login_window);
  EXPECT_FALSE(login_media_->session_running());
  EXPECT_TRUE(user_media_->session_running());
  const int before = seen_.frames;
  EXPECT_TRUE(WaitFor([&] { return seen_.frames > before; })) << "no pictures after the move";
  EXPECT_EQ(seen_.Ended(), "");

  // She logs out: back to the login window, on the same connection.
  SetConsole(0, "");
  ASSERT_TRUE(WaitFor([&] { return seen_.moves.load() == 2; }, 30s)) << "ended: " << seen_.Ended();
  EXPECT_TRUE(seen_.moved_to_login_window);
  EXPECT_TRUE(login_media_->session_running());
  EXPECT_FALSE(user_media_->session_running());
}

TEST_F(DaemonE2E, SomeoneElseLoggingInEndsTheSession) {
  ConnectResult r;
  session_ = RemoteSession::Start(Options("alice"), &r);
  ASSERT_TRUE(session_) << r.error << ": " << r.detail;
  SetConsole(getuid(), "bob");  // bob logs in at the Mac itself
  ASSERT_TRUE(WaitFor([&] { return !seen_.Ended().empty(); }, 10s));
  EXPECT_EQ(seen_.Ended(), "console_changed");
  EXPECT_EQ(seen_.moves.load(), 0);
  EXPECT_TRUE(WaitFor([&] { return !login_media_->session_running() && !user_media_->session_running(); }));
}

TEST_F(DaemonE2E, OnlyTheLoggedInUserMayLogInOnceSomeoneIs) {
  SetConsole(getuid(), "alice");
  ConnectResult bob = ConnectAndStartSession(Options("bob").control);
  EXPECT_EQ(bob.error, "not_console_user");
  ConnectResult r;
  session_ = RemoteSession::Start(Options("alice"), &r);
  ASSERT_TRUE(session_) << r.error << ": " << r.detail;
  EXPECT_FALSE(session_->session().login_window);
  EXPECT_TRUE(user_media_->session_running());
}

TEST_F(DaemonE2E, WithoutAnAgentForTheConsoleTheStartIsUnavailable) {
  user_agent_->Stop();
  ASSERT_TRUE(WaitFor([&] { return daemon_->agent_count() == 1; }));
  SetConsole(getuid(), "alice");
  ConnectResult r = ConnectAndStartSession(Options("alice").control);
  EXPECT_EQ(r.error, "unavailable");
}

TEST_F(DaemonE2E, APairingCodeLoginStaysOnTheDesktopItWasShownOn) {
  SetConsole(getuid(), "alice");
  RemoteSession::Options o = Options("alice");
  o.control.auth.method = AuthMethod::kPairingCode;
  o.control.auth.code = server_->IssuePairingCode();
  ConnectResult r;
  session_ = RemoteSession::Start(std::move(o), &r);
  ASSERT_TRUE(session_) << r.error << ": " << r.detail;
  EXPECT_TRUE(WaitFor([&] { return seen_.frames > 0; }));
  std::this_thread::sleep_for(1s);  // several CheckSession rounds
  EXPECT_EQ(seen_.Ended(), "");
  EXPECT_TRUE(user_media_->session_running());
  // alice logs out: the login window is no one's, so the session follows.
  SetConsole(0, "");
  ASSERT_TRUE(WaitFor([&] { return seen_.moves.load() == 1; }, 30s)) << "ended: " << seen_.Ended();
  EXPECT_TRUE(seen_.moved_to_login_window);
  // bob logs in there: not the desktop the code was for.
  SetConsole(getuid(), "bob");
  ASSERT_TRUE(WaitFor([&] { return !seen_.Ended().empty(); }, 10s));
  EXPECT_EQ(seen_.Ended(), "console_changed");
}
