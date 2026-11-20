// The agent's link to RDMachine Input (session/ime_bridge.h) with a fake
// input method on the other end of the socket: what it is sent, its acks,
// a commit nobody answers, and its reports. No input method, no keys.
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "rdmachine/agent_link.h"
#include "rdmachine/session/ime_bridge.h"

using namespace rdm;

namespace {

bool WaitUntil(const std::function<bool()>& done, int ms = 3000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return done();
}

struct Fixture {
  std::string dir, path;
  std::unique_ptr<ImeBridge> bridge;
  std::unique_ptr<AgentChannel> ime;  // the fake input method

  Fixture() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "rdm-ime-XXXXXX").string();
    dir = mkdtemp(tmpl.data());
    path = dir + "/sub/ime.sock";
    bridge = std::make_unique<ImeBridge>(path, nullptr, nullptr);
  }
  ~Fixture() {
    ime.reset();
    bridge.reset();
    std::filesystem::remove_all(dir);
  }
  void Connect() {
    std::string err;
    ASSERT_TRUE(bridge->Start(&err)) << err;
    const int fd = UnixConnect(path, &err);
    ASSERT_GE(fd, 0) << err;
    ime = std::make_unique<AgentChannel>(fd);
    ASSERT_TRUE(WaitUntil([&] { return bridge->connected(); }));
  }
  nlohmann::json Next() {
    AgentChannel::Received r = ime->Receive(2000);
    EXPECT_EQ(r.status, AgentChannel::Received::kMessage);
    return r.message;
  }
};

}  // namespace

TEST(ImeBridge, PassesTheCompositionOn) {
  Fixture f;
  f.Connect();
  f.bridge->Mark(3, "ni'hao", 6, 0);
  const nlohmann::json m = f.Next();
  EXPECT_EQ(m["type"], "mark");
  EXPECT_EQ(m["seq"], 3);
  EXPECT_EQ(m["text"], "ni'hao");
  EXPECT_EQ(m["sel"], nlohmann::json::array({6, 0}));
  f.bridge->Cancel(4);
  EXPECT_EQ(f.Next()["type"], "cancel");
}

TEST(ImeBridge, CommitWaitsForTheAck) {
  Fixture f;
  f.Connect();
  // No text field yet: nothing is sent, the caller types the text itself.
  EXPECT_FALSE(f.bridge->Commit(1, "你好", 100));
  f.ime->Send({{"type", "client"}, {"active", true}, {"app", "com.apple.TextEdit"}});
  ASSERT_TRUE(WaitUntil([&] { return f.bridge->client_active(); }));
  std::thread answer([&] {
    const nlohmann::json m = f.Next();
    EXPECT_EQ(m["type"], "commit");
    EXPECT_EQ(m["text"], "你好");
    f.ime->Send({{"type", "ack"}, {"seq", m["seq"]}, {"ok", true}});
  });
  EXPECT_TRUE(f.bridge->Commit(2, "你好", 2000));
  answer.join();
  // The input method could not put it in (its field went away).
  std::thread refuse([&] {
    const nlohmann::json m = f.Next();
    f.ime->Send({{"type", "ack"}, {"seq", m["seq"]}, {"ok", false}});
  });
  EXPECT_FALSE(f.bridge->Commit(3, "x", 2000));
  refuse.join();
}

TEST(ImeBridge, ACommitNobodyAnswersGivesUpInTime) {
  Fixture f;
  f.Connect();
  f.ime->Send({{"type", "client"}, {"active", true}, {"app", "a"}});
  ASSERT_TRUE(WaitUntil([&] { return f.bridge->client_active(); }));
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(f.bridge->Commit(1, "x", 100));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_GE(ms, 90);
  EXPECT_LT(ms, 1000);
}

TEST(ImeBridge, ReportsComeThroughAndAGoneInputMethodIsNoticed) {
  Fixture f;
  std::mutex mu;
  std::vector<ImeBridge::Report> reports;
  std::vector<std::string> clients;
  std::atomic<int> ended{0}, connections{0}, disconnections{0};
  ImeBridge::Events ev;
  ev.on_connection = [&](bool c) { ++(c ? connections : disconnections); };
  ev.on_client = [&](bool active, const std::string& app) {
    std::lock_guard lock(mu);
    clients.push_back((active ? "+" : "-") + app);
  };
  ev.on_report = [&](const ImeBridge::Report& r) {
    std::lock_guard lock(mu);
    reports.push_back(r);
  };
  ev.on_ended = [&](uint64_t) { ++ended; };
  f.bridge->SetEvents(ev);
  f.Connect();
  ASSERT_TRUE(WaitUntil([&] { return connections == 1; }));
  f.ime->Send({{"type", "client"}, {"active", true}, {"app", "com.apple.Safari"}});
  f.ime->Send({{"type", "state"}, {"seq", 5}, {"marked", {0, 2}}, {"sel", {2, 0}}, {"rect", {10.5, 20, 1, 18}}});
  f.ime->Send({{"type", "state"}, {"seq", 6}, {"marked", nullptr}, {"sel", {4, 0}}});
  f.ime->Send({{"type", "ended"}, {"seq", 6}});
  ASSERT_TRUE(WaitUntil([&] { return ended == 1; }));
  {
    std::lock_guard lock(mu);
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients[0], "+com.apple.Safari");
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].seq, 5u);
    ASSERT_TRUE(reports[0].marked);
    EXPECT_EQ(reports[0].marked->length, 2u);
    ASSERT_TRUE(reports[0].rect);
    EXPECT_DOUBLE_EQ(reports[0].rect->x, 10.5);
    EXPECT_DOUBLE_EQ(reports[0].rect->height, 18);
    EXPECT_FALSE(reports[1].marked);
    EXPECT_FALSE(reports[1].rect);
  }
  f.ime.reset();  // the input method quits
  ASSERT_TRUE(WaitUntil([&] { return disconnections == 1; }));
  EXPECT_FALSE(f.bridge->connected());
  EXPECT_FALSE(f.bridge->client_active());
  // It comes back (selected again): a new connection is taken.
  std::string err;
  const int fd = UnixConnect(f.path, &err);
  ASSERT_GE(fd, 0) << err;
  f.ime = std::make_unique<AgentChannel>(fd);
  ASSERT_TRUE(WaitUntil([&] { return connections == 2; }));
}

TEST(ImeBridge, SomeoneElsesProcessIsTurnedAway) {
  Fixture f;
  f.bridge = std::make_unique<ImeBridge>(f.path, [](int, std::string* why) {
    *why = "not ours";
    return false;
  }, nullptr);
  std::string err;
  ASSERT_TRUE(f.bridge->Start(&err)) << err;
  const int fd = UnixConnect(f.path, &err);
  ASSERT_GE(fd, 0) << err;
  AgentChannel ch(fd);
  EXPECT_EQ(ch.Receive(1000).status, AgentChannel::Received::kClosed);
  EXPECT_FALSE(f.bridge->connected());
}
