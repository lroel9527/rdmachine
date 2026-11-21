// The agent's link to RDMachine Input (session/ime_bridge.h) with a fake
// input method on the other end of the socket: what it is sent, its acks,
// a commit nobody answers, and its reports. No input method, no keys.
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>
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
  const uint64_t id = f.bridge->Mark(3, "ni'hao", 6, 0);
  const nlohmann::json m = f.Next();
  EXPECT_EQ(m["type"], "mark");
  EXPECT_EQ(m["seq"], 3);
  EXPECT_EQ(m["id"], id);
  EXPECT_EQ(m["text"], "ni'hao");
  EXPECT_EQ(m["sel"], nlohmann::json::array({6, 0}));
  const uint64_t cancel = f.bridge->Cancel(4);
  EXPECT_GT(cancel, id);
  const nlohmann::json c = f.Next();
  EXPECT_EQ(c["type"], "cancel");
  EXPECT_EQ(c["id"], cancel);
}

TEST(ImeBridge, CommitWaitsForTheAck) {
  Fixture f;
  f.Connect();
  // No text field yet: nothing is sent, the caller types the text itself.
  EXPECT_EQ(f.bridge->Commit(1, "你好", 100), ImeBridge::CommitResult::kNotTaken);
  f.ime->Send({{"type", "client"}, {"active", true}, {"app", "com.apple.TextEdit"}, {"ctl", 1}});
  ASSERT_TRUE(WaitUntil([&] { return f.bridge->client_active(); }));
  std::thread answer([&] {
    const nlohmann::json m = f.Next();
    EXPECT_EQ(m["type"], "commit");
    EXPECT_EQ(m["text"], "你好");
    f.ime->Send({{"type", "ack"}, {"seq", m["seq"]}, {"id", m["id"]}, {"ok", true}});
  });
  EXPECT_EQ(f.bridge->Commit(2, "你好", 2000), ImeBridge::CommitResult::kInserted);
  answer.join();
  // The input method could not put it in (its field went away).
  std::thread refuse([&] {
    const nlohmann::json m = f.Next();
    f.ime->Send({{"type", "ack"}, {"seq", m["seq"]}, {"id", m["id"]}, {"ok", false}});
  });
  EXPECT_EQ(f.bridge->Commit(3, "x", 2000), ImeBridge::CommitResult::kNotTaken);
  refuse.join();
}

TEST(ImeBridge, OnlyTheAnswerToThisVeryCommitCounts) {
  // Before 6.4.0 the bridge kept the highest number answered: after a
  // reconnect (the client numbers from 1 again) a commit with a lower number
  // counted as answered and refused at once, was typed as text, and the input
  // method put it in as well: the text twice.
  Fixture f;
  f.Connect();
  f.ime->Send({{"type", "client"}, {"active", true}, {"app", "a"}, {"ctl", 1}});
  ASSERT_TRUE(WaitUntil([&] { return f.bridge->client_active(); }));
  std::thread first([&] {
    const nlohmann::json m = f.Next();
    f.ime->Send({{"type", "ack"}, {"seq", m["seq"]}, {"id", m["id"]}, {"ok", true}});
  });
  EXPECT_EQ(f.bridge->Commit(777, "a", 2000), ImeBridge::CommitResult::kInserted);
  first.join();
  f.bridge->NewSession();
  std::thread second([&] {
    const nlohmann::json m = f.Next();
    // An answer to the old one again first, then to this one, a little later.
    f.ime->Send({{"type", "ack"}, {"seq", 777}, {"id", 1}, {"ok", false}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f.ime->Send({{"type", "ack"}, {"seq", m["seq"]}, {"id", m["id"]}, {"ok", true}});
  });
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(f.bridge->Commit(5, "b", 2000), ImeBridge::CommitResult::kInserted);
  EXPECT_GE(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(40));
  second.join();
}

TEST(ImeBridge, ACommitNobodyAnswersIsLeftToTheInputMethod) {
  Fixture f;
  std::mutex mu;
  std::vector<std::string> typed;
  ImeBridge::Events ev;
  ev.on_late_refusal = [&](const std::string& text) {
    std::lock_guard lock(mu);
    typed.push_back(text);
  };
  f.bridge->SetEvents(ev);
  f.Connect();
  f.ime->Send({{"type", "client"}, {"active", true}, {"app", "a"}, {"ctl", 1}});
  ASSERT_TRUE(WaitUntil([&] { return f.bridge->client_active(); }));
  const auto t0 = std::chrono::steady_clock::now();
  // Not typed: the input method has it and will put it in when it gets to it.
  EXPECT_EQ(f.bridge->Commit(1, "x", 100), ImeBridge::CommitResult::kNoAnswer);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_GE(ms, 90);
  EXPECT_LT(ms, 1000);
  const nlohmann::json m = f.Next();
  // Its late yes changes nothing; a late no hands the text back to be typed.
  f.ime->Send({{"type", "ack"}, {"seq", m["seq"]}, {"id", m["id"]}, {"ok", false}});
  ASSERT_TRUE(WaitUntil([&] {
    std::lock_guard lock(mu);
    return typed.size() == 1;
  }));
  std::lock_guard lock(mu);
  EXPECT_EQ(typed[0], "x");
}

TEST(ImeBridge, ReportsComeThroughAndAGoneInputMethodIsNoticed) {
  Fixture f;
  std::mutex mu;
  std::vector<ImeBridge::Report> reports;
  std::vector<std::string> clients;
  std::atomic<int> ended{0}, connections{0}, disconnections{0};
  std::atomic<uint64_t> ended_id{0};
  ImeBridge::Events ev;
  ev.on_connection = [&](bool c) { ++(c ? connections : disconnections); };
  ev.on_client = [&](bool active, const std::string& app, uint64_t ctl) {
    std::lock_guard lock(mu);
    clients.push_back((active ? "+" : "-") + app + "/" + std::to_string(ctl));
  };
  ev.on_report = [&](const ImeBridge::Report& r) {
    std::lock_guard lock(mu);
    reports.push_back(r);
  };
  ev.on_ended = [&](std::optional<uint64_t> id) {
    ended_id = id.value_or(0);
    ++ended;
  };
  f.bridge->SetEvents(ev);
  f.Connect();
  ASSERT_TRUE(WaitUntil([&] { return connections == 1; }));
  f.ime->Send({{"type", "client"}, {"active", true}, {"app", "com.apple.Safari"}, {"ctl", 4}});
  f.ime->Send({{"type", "state"}, {"seq", 5}, {"marked", {0, 2}}, {"sel", {2, 0}}, {"rect", {10.5, 20, 1, 18}}});
  f.ime->Send({{"type", "state"}, {"seq", 6}, {"marked", nullptr}, {"sel", {4, 0}}});
  f.ime->Send({{"type", "ended"}, {"seq", 6}, {"id", 12}});
  ASSERT_TRUE(WaitUntil([&] { return ended == 1; }));
  EXPECT_EQ(ended_id, 12u);
  EXPECT_EQ(f.bridge->client_ctl(), 4u);
  {
    std::lock_guard lock(mu);
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients[0], "+com.apple.Safari/4");
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

TEST(ImeBridge, AsksForAReportOnlyWhenAFieldHasTheFocus) {
  Fixture f;
  f.Connect();
  f.bridge->RequestReport(60);  // no field: nothing goes out
  f.ime->Send({{"type", "client"}, {"active", true}, {"app", "a"}});
  ASSERT_TRUE(WaitUntil([&] { return f.bridge->client_active(); }));
  f.bridge->RequestReport(60);
  const nlohmann::json m = f.Next();
  EXPECT_EQ(m["type"], "report");
  EXPECT_EQ(m["delay"], 60);
  EXPECT_FALSE(m.contains("seq"));  // it does not stand for a client message
}

TEST(ImeBridge, AReportOfTheFocusedFieldSaysAFieldHasTheFocus) {
  // A session that starts after the field got the focus never sees the
  // "client" message: each report says it again (6.3.6).
  Fixture f;
  std::mutex mu;
  std::vector<ImeBridge::Report> reports;
  ImeBridge::Events ev;
  ev.on_report = [&](const ImeBridge::Report& r) {
    std::lock_guard lock(mu);
    reports.push_back(r);
  };
  f.bridge->SetEvents(ev);
  f.Connect();
  EXPECT_FALSE(f.bridge->client_active());
  f.ime->Send({{"type", "state"}, {"seq", 1}, {"active", false}, {"app", "old"}, {"ctl", 2}, {"sel", {0, 0}}});
  f.ime->Send({{"type", "state"},
               {"seq", 2},
               {"active", true},
               {"app", "com.anthropic.claudefordesktop"},
               {"ctl", 3},
               {"sel", {3, 0}}});
  ASSERT_TRUE(WaitUntil([&] {
    std::lock_guard lock(mu);
    return reports.size() == 2;
  }));
  EXPECT_TRUE(f.bridge->client_active());
  EXPECT_EQ(f.bridge->client_app(), "com.anthropic.claudefordesktop");
  EXPECT_EQ(f.bridge->client_ctl(), 3u);
  std::lock_guard lock(mu);
  ASSERT_TRUE(reports[0].active);
  EXPECT_FALSE(*reports[0].active);
  ASSERT_TRUE(reports[1].active);
  EXPECT_TRUE(*reports[1].active);
  EXPECT_EQ(reports[1].app, "com.anthropic.claudefordesktop");
}

TEST(ImeBridge, AReportSaysWhichClientMessageItReflects) {
  Fixture f;
  std::mutex mu;
  std::vector<ImeBridge::Report> reports;
  ImeBridge::Events ev;
  ev.on_report = [&](const ImeBridge::Report& r) {
    std::lock_guard lock(mu);
    reports.push_back(r);
  };
  f.bridge->SetEvents(ev);
  f.Connect();
  f.bridge->NewSession();
  f.bridge->NoteClientMessage(1);  // the mode: does not go to the input method
  f.bridge->NoteClientMessage(2);
  const uint64_t a = f.bridge->Mark(3, "n", 1, 0);
  const uint64_t b = f.bridge->Mark(4, "ni", 2, 0);
  auto report_after = [&](uint64_t after) {
    f.ime->Send({{"type", "state"}, {"seq", 0}, {"after", after}, {"active", true}, {"app", "a"}, {"ctl", 1}});
  };
  report_after(0);  // made before mark 3 was put in: it reflects 1 and 2
  report_after(a);  // mark 3 in, mark 4 not yet
  report_after(b);  // all of it
  ASSERT_TRUE(WaitUntil([&] {
    std::lock_guard lock(mu);
    return reports.size() == 3;
  }));
  f.bridge->NewSession();  // the client numbers from 1 again
  const uint64_t c = f.bridge->Mark(1, "x", 1, 0);
  report_after(b);  // made before this session's first message was put in
  report_after(c);
  f.ime->Send({{"type", "state"}, {"seq", 0}, {"active", true}, {"app", "a"}});  // an input method before 6.4.0
  ASSERT_TRUE(WaitUntil([&] {
    std::lock_guard lock(mu);
    return reports.size() == 6;
  }));
  std::lock_guard lock(mu);
  EXPECT_EQ(reports[0].answers, 2u);
  EXPECT_EQ(reports[1].answers, 3u);
  EXPECT_EQ(reports[2].answers, 4u);
  EXPECT_EQ(reports[3].answers, 0u);
  EXPECT_EQ(reports[4].answers, 1u);
  EXPECT_FALSE(reports[5].answers);
  EXPECT_EQ(reports[4].ctl, 1u);
}
