#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "fake_clipboard.h"
#include "rdmachine/clipboard_sync.h"

using namespace rdm;
using namespace std::chrono_literals;

namespace {

// One side of a session driven by hand: its clipboard, what it sent.
struct Side {
  explicit Side(ClipboardSync::Options o = {}) : sync(clipboard, [this](const std::string& t) { return Send(t); }, o) {}

  bool Send(const std::string& t) {
    if (!connected) return false;
    sent.push_back(t);
    return true;
  }
  void Receive(const std::string& t) { sync.OnRemoteText(reinterpret_cast<const uint8_t*>(t.data()), t.size()); }

  std::shared_ptr<FakeClipboard> clipboard = std::make_shared<FakeClipboard>();
  bool connected = true;
  std::vector<std::string> sent;
  ClipboardSync sync;
};

ClipboardSync::Options Mode(ClipboardMode m) {
  ClipboardSync::Options o;
  o.mode = m;
  return o;
}

// Delivers everything each side sent to the other, then lets both poll.
void Exchange(Side& a, Side& b) {
  for (int round = 0; round < 3; ++round) {
    for (auto& t : a.sent) b.Receive(t);
    for (auto& t : b.sent) a.Receive(t);
    a.sent.clear();
    b.sent.clear();
    a.sync.PollOnce();
    b.sync.PollOnce();
  }
}

}  // namespace

TEST(Utf8, AcceptsWellFormed) {
  EXPECT_TRUE(IsValidUtf8(""));
  EXPECT_TRUE(IsValidUtf8("hello"));
  EXPECT_TRUE(IsValidUtf8("剪贴板 ✓ 😀"));
  EXPECT_TRUE(IsValidUtf8("\xF4\x8F\xBF\xBF"));  // U+10FFFF
}

TEST(Utf8, RefusesMalformed) {
  EXPECT_FALSE(IsValidUtf8("\x80"));
  EXPECT_FALSE(IsValidUtf8("\xC3"));              // truncated
  EXPECT_FALSE(IsValidUtf8("\xC0\xAF"));          // overlong '/'
  EXPECT_FALSE(IsValidUtf8("\xE0\x80\xAF"));      // overlong
  EXPECT_FALSE(IsValidUtf8("\xED\xA0\x80"));      // surrogate U+D800
  EXPECT_FALSE(IsValidUtf8("\xF4\x90\x80\x80"));  // above U+10FFFF
  EXPECT_FALSE(IsValidUtf8("\xFF"));
  EXPECT_FALSE(IsValidUtf8("ok\xE4\xB8"));        // truncated at the end
}

TEST(ClipboardMode, ParsesNames) {
  for (ClipboardMode m : {ClipboardMode::kBoth, ClipboardMode::kSend, ClipboardMode::kReceive, ClipboardMode::kOff}) {
    EXPECT_EQ(ParseClipboardMode(ClipboardModeName(m)), m);
  }
  EXPECT_FALSE(ParseClipboardMode("sometimes"));
}

TEST(ClipboardSync, LocalCopyIsSentOnce) {
  Side s;
  s.sync.Begin();
  s.sync.PollOnce();
  EXPECT_TRUE(s.sent.empty());
  s.clipboard->Copy("hello");
  s.sync.PollOnce();
  s.sync.PollOnce();
  EXPECT_EQ(s.sent, std::vector<std::string>{"hello"});
}

TEST(ClipboardSync, WhatWasThereBeforeIsNotSentByDefault) {
  Side s;
  s.clipboard->Copy("old");
  s.sync.Begin();
  s.sync.PollOnce();
  EXPECT_TRUE(s.sent.empty());
}

TEST(ClipboardSync, SendOnStartSendsWhatIsThere) {
  ClipboardSync::Options o;
  o.send_on_start = true;
  Side s(o);
  s.clipboard->Copy("copied before connecting");
  s.sync.Begin();
  s.sync.PollOnce();
  EXPECT_EQ(s.sent, std::vector<std::string>{"copied before connecting"});
}

TEST(ClipboardSync, RemoteTextIsWrittenAndNotEchoed) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("from a");
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->text(), "from a");
  // Nothing went back and forth after the first copy.
  a.sync.PollOnce();
  b.sync.PollOnce();
  EXPECT_TRUE(a.sent.empty());
  EXPECT_TRUE(b.sent.empty());
  EXPECT_EQ(b.clipboard->writes(), 1);
  EXPECT_EQ(a.clipboard->writes(), 0);

  // And the other way.
  b.clipboard->Copy("from b");
  Exchange(a, b);
  EXPECT_EQ(a.clipboard->text(), "from b");
  EXPECT_EQ(a.clipboard->writes(), 1);
  EXPECT_EQ(b.clipboard->writes(), 1);
}

TEST(ClipboardSync, CopyingTheSameTextAgainAfterThePeerChangedItIsSent) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("x");
  Exchange(a, b);
  b.clipboard->Copy("y");
  Exchange(a, b);
  a.clipboard->Copy("x");
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->text(), "x");
}

TEST(ClipboardSync, RetriesUntilConnected) {
  Side s;
  s.connected = false;
  s.sync.Begin();
  s.clipboard->Copy("waiting");
  s.sync.PollOnce();
  s.sync.PollOnce();
  EXPECT_TRUE(s.sent.empty());
  s.connected = true;
  s.sync.PollOnce();
  EXPECT_EQ(s.sent, std::vector<std::string>{"waiting"});
}

TEST(ClipboardSync, ConcealedEmptyAndNonTextAreNotSent) {
  Side s;
  s.sync.Begin();
  s.clipboard->Copy("hunter2", /*concealed=*/true);
  s.sync.PollOnce();
  s.clipboard->Copy("");
  s.sync.PollOnce();
  s.clipboard->CopyNonText();
  s.sync.PollOnce();
  EXPECT_TRUE(s.sent.empty());
}

TEST(ClipboardSync, SizeLimitBothWays) {
  ClipboardSync::Options o;
  o.max_bytes = 8;
  Side s(o);
  s.sync.Begin();
  s.clipboard->Copy("123456789");
  s.sync.PollOnce();
  EXPECT_TRUE(s.sent.empty());
  s.Receive("abcdefghi");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->text(), "123456789");
  s.Receive("abcdefgh");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->text(), "abcdefgh");
}

TEST(ClipboardSync, InvalidUtf8FromThePeerIsIgnored) {
  Side s;
  s.sync.Begin();
  s.Receive("bad\xFF");
  s.Receive("");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->writes(), 0);
}

TEST(ClipboardSync, NewestRemoteTextWins) {
  Side s;
  s.sync.Begin();
  s.Receive("first");
  s.Receive("second");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->text(), "second");
  EXPECT_EQ(s.clipboard->writes(), 1);
}

TEST(ClipboardSync, ModesLimitTheDirection) {
  Side send_only(Mode(ClipboardMode::kSend));
  send_only.sync.Begin();
  send_only.Receive("in");
  send_only.clipboard->Copy("out");
  send_only.sync.PollOnce();
  EXPECT_EQ(send_only.clipboard->text(), "out");
  EXPECT_EQ(send_only.sent, std::vector<std::string>{"out"});

  Side receive_only(Mode(ClipboardMode::kReceive));
  receive_only.sync.Begin();
  receive_only.clipboard->Copy("out");
  receive_only.sync.PollOnce();
  EXPECT_TRUE(receive_only.sent.empty());
  receive_only.Receive("in");
  receive_only.sync.PollOnce();
  EXPECT_EQ(receive_only.clipboard->text(), "in");
  EXPECT_TRUE(receive_only.sent.empty());

  Side off(Mode(ClipboardMode::kOff));
  off.sync.Begin();
  off.Receive("in");
  off.clipboard->Copy("out");
  off.sync.PollOnce();
  EXPECT_EQ(off.clipboard->text(), "out");
  EXPECT_TRUE(off.sent.empty());
}

TEST(ClipboardSync, ThreadedBothWays) {
  // Two syncs wired to each other through their send functions, polling on
  // their own threads.
  auto ca = std::make_shared<FakeClipboard>(), cb = std::make_shared<FakeClipboard>();
  std::atomic<int> sends{0};
  ClipboardSync::Options o;
  o.poll_ms = 10;
  ClipboardSync* pa = nullptr;
  ClipboardSync* pb = nullptr;
  ClipboardSync a(ca, [&](const std::string& t) {
    ++sends;
    pb->OnRemoteText(reinterpret_cast<const uint8_t*>(t.data()), t.size());
    return true;
  }, o);
  ClipboardSync b(cb, [&](const std::string& t) {
    ++sends;
    pa->OnRemoteText(reinterpret_cast<const uint8_t*>(t.data()), t.size());
    return true;
  }, o);
  pa = &a;
  pb = &b;
  a.Start();
  b.Start();
  auto wait = [](auto f) {
    for (int i = 0; i < 300 && !f(); ++i) std::this_thread::sleep_for(10ms);
    return f();
  };
  ca->Copy("one");
  EXPECT_TRUE(wait([&] { return cb->text() == "one"; }));
  cb->Copy("two");
  EXPECT_TRUE(wait([&] { return ca->text() == "two"; }));
  std::this_thread::sleep_for(100ms);  // no echo storm afterwards
  EXPECT_EQ(sends, 2);
  a.Stop();
  b.Stop();
}
