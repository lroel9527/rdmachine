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


struct Msg {
  uint32_t type;
  std::string payload;
};

// The hand-driven tests' clock: Exchange moves it on 100 ms a round.
uint64_t g_now_us = 1'000'000;

ClipboardSync::Options Fast(ClipboardSync::Options o = {}) {
  o.hello_wait_ms = 0;
  o.pacer.start_bps = 1e12;  // no pacing in the hand-driven tests
  return o;
}

// One side of a session driven by hand: its clipboard, what it sent.
struct Side {
  explicit Side(ClipboardSync::Options o = Fast())
      : sync(clipboard, [this](uint32_t type, std::string_view p) { return Send(type, p); }, o) {}

  bool Send(uint32_t type, std::string_view p) {
    if (!connected) return false;
    sent.push_back({type, std::string(p)});
    log.push_back({type, std::string(p)});
    return true;
  }
  void Receive(uint32_t type, const std::string& p) {
    sync.OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
  }
  void ReceiveText(const std::string& t) { Receive(kClipboardTextMessageType, t); }
  // Everything this side ever sent, of one type.
  std::vector<std::string> Logged(uint32_t type) const {
    std::vector<std::string> out;
    for (const auto& m : log) {
      if (m.type == type) out.push_back(m.payload);
    }
    return out;
  }
  // Everything but the hello.
  size_t LoggedCount() const {
    size_t n = 0;
    for (const auto& m : log) n += m.type != kClipboardHelloMessageType;
    return n;
  }
  uint64_t DataBytes() const {
    uint64_t n = 0;
    for (const auto& p : Logged(kClipboardDataMessageType)) n += p.size() - kClipboardDataHeaderSize;
    return n;
  }

  std::shared_ptr<FakeClipboard> clipboard = std::make_shared<FakeClipboard>();
  bool connected = true;
  std::vector<Msg> sent;  // not yet delivered
  std::vector<Msg> log;   // all
  ClipboardSync sync;
};

ClipboardSync::Options Mode(ClipboardMode m) {
  ClipboardSync::Options o = Fast();
  o.mode = m;
  return o;
}

// Delivers what each side sent to the other, then lets both poll.
bool Round(Side& a, Side& b) {
  g_now_us += 100'000;
  std::vector<Msg> to_b, to_a;
  to_b.swap(a.sent);
  to_a.swap(b.sent);
  for (auto& m : to_b) b.Receive(m.type, m.payload);
  for (auto& m : to_a) a.Receive(m.type, m.payload);
  a.sync.PollOnce();
  b.sync.PollOnce();
  return !to_a.empty() || !to_b.empty();
}

// Until nothing moves.
void Exchange(Side& a, Side& b, int rounds = 200) {
  for (int round = 0; round < rounds; ++round) {
    if (!Round(a, b) && a.sent.empty() && b.sent.empty() && round > 2) return;
  }
}

// An application on `at` pastes `mime`: the promise fetches it from
// `other`, the exchange running while it waits.
std::optional<std::string> Paste(Side& at, Side& other, std::string_view mime) {
  auto p = at.clipboard->promise();
  if (!p) return std::nullopt;
  return p->FetchFormat(std::string(mime), [&] { Round(at, other); }, 5000);
}

std::string Pattern(size_t n, int seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131 + seed * 7 + i / 977) & 0xFF);
  return s;
}

ClipboardSync::Options Server(ClipboardSync::Options o = Fast()) {
  o.push_text_to_older_peers = true;
  return o;
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

// ---- nothing leaves before a paste ----------------------------------------

TEST(ClipboardSync, HelloGoesFirst) {
  Side s;
  s.sync.Begin();
  s.sync.PollOnce();
  ASSERT_EQ(s.sent.size(), 1u);
  EXPECT_EQ(s.sent[0].type, kClipboardHelloMessageType);
  auto h = DecodeClipboardHello(s.sent[0].payload);
  ASSERT_TRUE(h);
  EXPECT_EQ(h->version, 3);
}

TEST(ClipboardSync, ACopyIsOnlyAnnouncedNotSent) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  Exchange(a, b);
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "my secret password"}, {std::string(kMimePng), Pattern(1000, 1)}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  // One announcement: the formats, and nothing of the content.
  const auto announces = a.Logged(kClipboardAnnounceMessageType);
  ASSERT_EQ(announces.size(), 1u);
  EXPECT_EQ(announces[0].find("secret"), std::string::npos);
  auto ann = DecodeClipboardAnnounce(announces[0]);
  ASSERT_TRUE(ann);
  EXPECT_EQ(ann->formats, (std::vector<std::string>{"text/plain", "image/png"}));
  EXPECT_EQ(a.LoggedCount(), 1u);
  EXPECT_EQ(a.DataBytes(), 0u);
  for (const auto& m : a.log) EXPECT_EQ(m.payload.find("secret"), std::string::npos);
  // The other side holds a promise of it.
  auto p = b.clipboard->promise();
  ASSERT_TRUE(p);
  EXPECT_EQ(p->formats(), (std::vector<std::string>{"text/plain", "image/png"}));
  EXPECT_FALSE(b.clipboard->text());
}

TEST(ClipboardSync, PastingFetchesOnlyWhatIsPasted) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  Exchange(a, b);
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "hello ✓"}, {std::string(kMimePng), Pattern(700'000, 2)}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_EQ(Paste(b, a, kMimeText), "hello ✓");
  EXPECT_EQ(a.DataBytes(), std::string("hello ✓").size());  // not the image
  // Pasting again does not fetch again.
  const size_t requests = b.Logged(kClipboardRequestMessageType).size();
  EXPECT_EQ(Paste(b, a, kMimeText), "hello ✓");
  EXPECT_EQ(b.Logged(kClipboardRequestMessageType).size(), requests);
  // The image when it is pasted somewhere that takes images.
  EXPECT_EQ(Paste(b, a, kMimePng), Pattern(700'000, 2));
}

TEST(ClipboardSync, NothingCopiedBeforeConnectingIsAnnounced) {
  Side a, b;
  a.clipboard->Copy("copied before connecting");
  a.sync.Begin();
  b.sync.Begin();
  Exchange(a, b);
  EXPECT_TRUE(a.Logged(kClipboardAnnounceMessageType).empty());
  EXPECT_FALSE(b.clipboard->promise());
}

TEST(ClipboardSync, RichTextGoesAcross) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "bold"},
               {std::string(kMimeRtf), "{\\rtf1 \\b bold}"},
               {std::string(kMimeHtml), "<b>bold</b>"}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_EQ(Paste(b, a, kMimeRtf), "{\\rtf1 \\b bold}");
  EXPECT_EQ(Paste(b, a, kMimeHtml), "<b>bold</b>");
  EXPECT_FALSE(Paste(b, a, "application/x-unknown"));
}

TEST(ClipboardSync, LargeTextGoesAcrossInPieces) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  std::string big(6u << 20, 'x');  // more than one plain text message allows
  a.clipboard->Copy(big);
  Exchange(a, b);
  auto got = Paste(b, a, kMimeText);
  ASSERT_TRUE(got);
  EXPECT_TRUE(*got == big);
}

TEST(ClipboardSync, UnknownFormatsAreNotAnnounced) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{"application/x-secret", "zzz"}, {std::string(kMimeText), "plain"}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  ASSERT_TRUE(b.clipboard->promise());
  EXPECT_EQ(b.clipboard->promise()->formats(), (std::vector<std::string>{"text/plain"}));
}

TEST(ClipboardSync, ThePromiseIsNotAnnouncedBack) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("from a");
  Exchange(a, b);
  ASSERT_EQ(Paste(b, a, kMimeText), "from a");
  Exchange(a, b);
  EXPECT_TRUE(b.Logged(kClipboardAnnounceMessageType).empty());
  EXPECT_EQ(a.clipboard->writes(), 0);

  // And the other way.
  b.clipboard->Copy("from b");
  Exchange(a, b);
  EXPECT_EQ(Paste(a, b, kMimeText), "from b");
}

TEST(ClipboardSync, ANewerCopyVoidsThePromise) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("old");
  Exchange(a, b);
  auto old = b.clipboard->promise();
  a.clipboard->Copy("new");
  Exchange(a, b);
  EXPECT_FALSE(old->FetchFormat(std::string(kMimeText), [&] { Round(b, a); }, 2000));
  EXPECT_EQ(Paste(b, a, kMimeText), "new");
}

TEST(ClipboardSync, ACopyOnThePastingSideVoidsThePromise) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("theirs");
  Exchange(a, b);
  auto p = b.clipboard->promise();
  b.clipboard->Copy("mine");
  Exchange(a, b);
  EXPECT_FALSE(p->FetchFormat(std::string(kMimeText), [&] { Round(b, a); }, 2000));
  EXPECT_EQ(Paste(a, b, kMimeText), "mine");
}

TEST(ClipboardSync, APromiseGivesUpWhenThePeerIsSilent) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("x");
  Exchange(a, b);
  auto p = b.clipboard->promise();
  // b's request never reaches a.
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(p->FetchFormat(std::string(kMimeText), [&] { b.sync.PollOnce(); b.sent.clear(); }, 200));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
}

TEST(ClipboardSync, StoppingVoidsPromises) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("x");
  Exchange(a, b);
  auto p = b.clipboard->promise();
  b.sync.Stop();
  EXPECT_FALSE(p->FetchFormat(std::string(kMimeText), nullptr, 5000));
}

TEST(ClipboardSync, UnaskedOffersAreRefused) {
  Side b;
  b.sync.Begin();
  b.sync.PollOnce();
  b.Receive(kClipboardHelloMessageType, EncodeClipboardHello({3}));
  ClipboardOffer o{5, 77, "text/plain", 3, ""};
  b.Receive(kClipboardOfferMessageType, EncodeClipboardOffer(o));
  b.Receive(kClipboardDataMessageType, EncodeClipboardData({5, 0}, "abc"));
  b.sync.PollOnce();
  EXPECT_EQ(b.clipboard->writes(), 0);
  auto cancels = b.Logged(kClipboardCancelMessageType);
  ASSERT_EQ(cancels.size(), 1u);
  EXPECT_EQ(DecodeClipboardCancel(cancels[0])->reason, "refused");
}

TEST(ClipboardSync, FetchOnAnnounceWritesEverything) {
  ClipboardSync::Options eager = Fast();
  eager.fetch_on_announce = true;
  Side a, b(eager);
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "now"}, {std::string(kMimeHtml), "<i>now</i>"}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->content(), c);
  EXPECT_FALSE(b.clipboard->promise());
}

TEST(ClipboardSync, AClipboardWithoutPromisesGetsEverything) {
  Side a, b;
  b.clipboard->set_can_promise(false);
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("all at once");
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->text(), "all at once");
}

TEST(ClipboardSync, ModesLimitTheDirection) {
  // A side that does not receive keeps no promise and fetches nothing.
  Side a, send_only(Mode(ClipboardMode::kSend));
  a.sync.Begin();
  send_only.sync.Begin();
  a.clipboard->Copy("not wanted");
  Exchange(a, send_only);
  EXPECT_FALSE(send_only.clipboard->promise());
  EXPECT_EQ(send_only.clipboard->writes(), 0);
  send_only.clipboard->Copy("out");
  Exchange(a, send_only);
  EXPECT_EQ(Paste(a, send_only, kMimeText), "out");

  // A side that does not send announces nothing.
  Side b, receive_only(Mode(ClipboardMode::kReceive));
  b.sync.Begin();
  receive_only.sync.Begin();
  receive_only.clipboard->Copy("stays here");
  Exchange(b, receive_only);
  EXPECT_TRUE(receive_only.Logged(kClipboardAnnounceMessageType).empty());
  b.clipboard->Copy("in");
  Exchange(b, receive_only);
  EXPECT_EQ(Paste(receive_only, b, kMimeText), "in");
}

TEST(ClipboardSync, ThreadedBothWays) {
  // Two syncs wired to each other on their own threads; pastes on the
  // test's thread, as an application would.
  auto ca = std::make_shared<FakeClipboard>(), cb = std::make_shared<FakeClipboard>();
  ClipboardSync::Options o;
  o.poll_ms = 10;
  ClipboardSync* pa = nullptr;
  ClipboardSync* pb = nullptr;
  ClipboardSync a(ca, [&](uint32_t type, std::string_view p) {
    pb->OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
    return true;
  }, o);
  ClipboardSync b(cb, [&](uint32_t type, std::string_view p) {
    pa->OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
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
  ASSERT_TRUE(wait([&] { return cb->promise() != nullptr; }));
  EXPECT_EQ(cb->promise()->FetchFormat(std::string(kMimeText), nullptr), "one");
  cb->Copy("two");
  ASSERT_TRUE(wait([&] { return ca->promise() != nullptr; }));
  EXPECT_EQ(ca->promise()->FetchFormat(std::string(kMimeText), nullptr), "two");
  a.Stop();
  b.Stop();
}

// ---- with a peer of an older version (plain text messages only) ----------

TEST(ClipboardSync, TheServerGivesAnOlderPeerPlainTextAtOnce) {
  Side s(Server());
  s.sync.Begin();
  s.sync.PollOnce();
  s.clipboard->Copy("hello");
  s.sync.PollOnce();
  s.sync.PollOnce();
  EXPECT_EQ(s.Logged(kClipboardTextMessageType), std::vector<std::string>{"hello"});
  EXPECT_EQ(s.LoggedCount(), 1u);
}

TEST(ClipboardSync, TheClientGivesAnOlderPeerNothing) {
  Side s;  // push_text_to_older_peers off
  s.sync.Begin();
  s.sync.PollOnce();
  s.clipboard->Copy("hello");
  s.sync.PollOnce();
  s.sync.PollOnce();
  EXPECT_EQ(s.LoggedCount(), 0u);
}

TEST(ClipboardSync, AnEarlierVersionIsAnOlderPeer) {
  Side s(Server());
  s.sync.Begin();
  s.sync.PollOnce();
  s.Receive(kClipboardHelloMessageType, EncodeClipboardHello({2}));
  s.clipboard->Copy("hello");
  s.sync.PollOnce();
  EXPECT_EQ(s.Logged(kClipboardTextMessageType), std::vector<std::string>{"hello"});
}

TEST(ClipboardSync, WaitsForThePeersHelloBeforeFallingBack) {
  ClipboardSync::Options o = Server();
  o.hello_wait_ms = 60'000;
  Side s(o);
  s.sync.Begin();
  s.sync.PollOnce();
  s.clipboard->Copy("hello");
  s.sync.PollOnce();
  EXPECT_EQ(s.LoggedCount(), 0u);  // the peer may yet say hello
  s.Receive(kClipboardHelloMessageType, EncodeClipboardHello({3}));
  s.sync.PollOnce();
  EXPECT_EQ(s.Logged(kClipboardAnnounceMessageType).size(), 1u);  // it did: version 3
  EXPECT_TRUE(s.Logged(kClipboardTextMessageType).empty());
}

TEST(ClipboardSync, OlderPeersTextIsWritten) {
  Side s;
  s.sync.Begin();
  s.ReceiveText("from an older peer");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->text(), "from an older peer");
  s.sync.PollOnce();
  EXPECT_EQ(s.LoggedCount(), 0u);  // not echoed
}

TEST(ClipboardSync, RetriesUntilConnected) {
  Side s(Server());
  s.connected = false;
  s.sync.Begin();
  s.clipboard->Copy("waiting");
  s.sync.PollOnce();
  s.sync.PollOnce();
  EXPECT_TRUE(s.log.empty());
  s.connected = true;
  s.sync.PollOnce();  // hello
  s.sync.PollOnce();
  EXPECT_EQ(s.Logged(kClipboardTextMessageType), std::vector<std::string>{"waiting"});
}

TEST(ClipboardSync, ConcealedAndEmptyAreNotAnnounced) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  Exchange(a, b);
  a.clipboard->Copy("hunter2", /*concealed=*/true);
  Exchange(a, b);
  a.clipboard->Copy("");
  Exchange(a, b);
  a.clipboard->CopyNothingShared();
  Exchange(a, b);
  EXPECT_EQ(a.LoggedCount(), 0u);
}

TEST(ClipboardSync, PlainTextSizeLimitBothWays) {
  ClipboardSync::Options o = Server();
  o.max_text_bytes = 8;
  Side s(o);
  s.sync.Begin();
  s.clipboard->Copy("123456789");
  s.sync.PollOnce();
  s.sync.PollOnce();
  EXPECT_EQ(s.LoggedCount(), 0u);
  s.ReceiveText("abcdefghi");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->text(), "123456789");
  s.ReceiveText("abcdefgh");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->text(), "abcdefgh");
}

TEST(ClipboardSync, InvalidUtf8FromThePeerIsIgnored) {
  Side s;
  s.sync.Begin();
  s.ReceiveText("bad\xFF");
  s.ReceiveText("");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->writes(), 0);
}

TEST(ClipboardSync, OtherMessagesAreNotTaken) {
  EXPECT_FALSE(ClipboardSync::IsClipboardMessage(0x10002));
  EXPECT_FALSE(ClipboardSync::IsClipboardMessage(0));
  EXPECT_TRUE(ClipboardSync::IsClipboardMessage(kClipboardTextMessageType));
  EXPECT_TRUE(ClipboardSync::IsClipboardMessage(kClipboardAnnounceMessageType));
  EXPECT_TRUE(ClipboardSync::IsClipboardMessage(kClipboardRequestMessageType));
}

// ---- large pastes ---------------------------------------------------------

// A copying side paced as on a real link (8 Mbps to start), on the test clock.
ClipboardSync::Options Paced() {
  ClipboardSync::Options o = Fast();
  o.pacer = TransferPacer::Options{};
  o.clock = [] { return g_now_us; };
  return o;
}

TEST(ClipboardSync, CopiedFilesGoOnlyAsTheirNames) {
  // The in-memory clipboard stands for what macOS reads after a Finder copy:
  // the names as text, nothing else.
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("report.pdf\nphotos");
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->promise()->formats(), (std::vector<std::string>{"text/plain"}));
  EXPECT_EQ(Paste(b, a, kMimeText), "report.pdf\nphotos");
}

TEST(ClipboardSync, ANewerCopyStopsAPasteOnItsWay) {
  Side a(Paced()), b;
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimePng), Pattern(2'000'000, 5)}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  auto p = b.clipboard->promise();
  int rounds = 0;
  // Three rounds into the paste, a copies something else.
  auto got = p->FetchFormat(std::string(kMimePng), [&] {
    if (++rounds == 3) a.clipboard->Copy("never mind");
    Round(b, a);
  }, 5000);
  EXPECT_FALSE(got);
  Exchange(a, b);
  EXPECT_EQ(Paste(b, a, kMimeText), "never mind");
}

TEST(ClipboardSync, OverTheLimitAPasteGetsNothing) {
  ClipboardSync::Options small = Fast();
  small.max_format_bytes = 1000;
  Side a(small), b;
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "short"}, {std::string(kMimePng), Pattern(2000, 1)}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_FALSE(Paste(b, a, kMimePng));
  EXPECT_EQ(a.DataBytes(), 0u);
  EXPECT_EQ(Paste(b, a, kMimeText), "short");
}

TEST(ClipboardSync, ThePastingSidesLimitAppliesToo) {
  ClipboardSync::Options small = Fast();
  small.max_format_bytes = 1000;
  Side a, b(small);
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimePng), Pattern(2000, 1)}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_FALSE(Paste(b, a, kMimePng));
}

TEST(ClipboardSync, DataOutOfOrderDropsThePaste) {
  Side b;
  b.sync.Begin();
  b.sync.PollOnce();
  b.Receive(kClipboardHelloMessageType, EncodeClipboardHello({3}));
  b.Receive(kClipboardAnnounceMessageType, EncodeClipboardAnnounce({1, {"text/plain"}}));
  b.sync.PollOnce();
  auto p = b.clipboard->promise();
  ASSERT_TRUE(p);
  bool answered = false;
  auto got = p->FetchFormat(std::string(kMimeText), [&] {
    b.sync.PollOnce();
    if (answered) return;
    for (const auto& m : b.log) {
      if (m.type != kClipboardRequestMessageType) continue;
      answered = true;
      const uint64_t req = DecodeClipboardRequest(m.payload)->id;
      b.Receive(kClipboardOfferMessageType, EncodeClipboardOffer({9, req, "text/plain", 4, ""}));
      b.Receive(kClipboardDataMessageType, EncodeClipboardData({9, 2}, "zz"));  // skips bytes 0-1
    }
  }, 2000);
  EXPECT_FALSE(got);
  bool cancelled = false;
  for (const auto& m : b.log) {
    if (m.type == kClipboardCancelMessageType) {
      auto c = DecodeClipboardCancel(m.payload);
      cancelled = c && c->id == 9 && !c->from_sender && c->reason == "error";
    }
  }
  EXPECT_TRUE(cancelled);
}

TEST(ClipboardSync, AVersion2FileRequestGetsNothing) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("x");
  Exchange(a, b);
  a.Receive(kClipboardRequestMessageType, R"({"announce":1,"id":50,"files":true})");
  a.sync.PollOnce();
  EXPECT_EQ(a.DataBytes(), 0u);
}

TEST(ClipboardSync, ThreadedWithPacing) {
  // Real threads and the real pacer: a 3 MB image arrives intact when pasted.
  auto ca = std::make_shared<FakeClipboard>(), cb = std::make_shared<FakeClipboard>();
  ClipboardSync::Options o;
  o.poll_ms = 10;
  ClipboardSync* pa = nullptr;
  ClipboardSync* pb = nullptr;
  ClipboardSync a(ca, [&](uint32_t type, std::string_view p) {
    pb->OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
    return true;
  }, o);
  ClipboardSync b(cb, [&](uint32_t type, std::string_view p) {
    pa->OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
    return true;
  }, o);
  pa = &a;
  pb = &b;
  a.Start();
  b.Start();
  ClipboardContent c;
  c.formats = {{std::string(kMimePng), Pattern(3'000'000, 6)}};
  ca->CopyContent(c);
  for (int i = 0; i < 300 && !cb->promise(); ++i) std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(cb->promise());
  int pumps = 0;
  auto got = cb->promise()->FetchFormat(std::string(kMimePng), [&] { ++pumps; });
  ASSERT_TRUE(got);
  EXPECT_TRUE(*got == Pattern(3'000'000, 6));
  EXPECT_GT(pumps, 0);  // the waiting side kept pumping
  a.Stop();
  b.Stop();
}
