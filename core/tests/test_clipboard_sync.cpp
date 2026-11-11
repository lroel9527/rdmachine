#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "fake_clipboard.h"
#include "rdmachine/clipboard_sync.h"

using namespace rdm;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

OfferEntry File(std::string path, uint64_t size, uint32_t mode = 0, int64_t mtime = 0) {
  return OfferEntry{std::move(path), OfferEntry::Kind::kFile, size, mode, mtime, {}};
}
OfferEntry Dir(std::string path, uint32_t mode = 0, int64_t mtime = 0) {
  return OfferEntry{std::move(path), OfferEntry::Kind::kDir, 0, mode, mtime, {}};
}
}  // namespace

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
    return true;
  }
  void Receive(uint32_t type, const std::string& p) {
    sync.OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
  }
  void ReceiveText(const std::string& t) { Receive(kClipboardTextMessageType, t); }
  // What went out, without the hello.
  std::vector<Msg> Sent() const {
    std::vector<Msg> out;
    for (const auto& m : sent) {
      if (m.type != kClipboardHelloMessageType) out.push_back(m);
    }
    return out;
  }
  std::vector<std::string> SentTexts() const {
    std::vector<std::string> out;
    for (const auto& m : sent) {
      if (m.type == kClipboardTextMessageType) out.push_back(m.payload);
    }
    return out;
  }
  std::vector<ClipboardOffer> SentOffers() const {
    std::vector<ClipboardOffer> out;
    for (const auto& m : sent) {
      if (m.type == kClipboardOfferMessageType) {
        if (auto o = DecodeClipboardOffer(m.payload)) out.push_back(*o);
      }
    }
    return out;
  }

  std::shared_ptr<FakeClipboard> clipboard = std::make_shared<FakeClipboard>();
  bool connected = true;
  std::vector<Msg> sent;
  ClipboardSync sync;
};

ClipboardSync::Options Mode(ClipboardMode m) {
  ClipboardSync::Options o = Fast();
  o.mode = m;
  return o;
}

// Delivers everything each side sent to the other, then lets both poll,
// until nothing moves.
void Exchange(Side& a, Side& b, int rounds = 200) {
  for (int round = 0; round < rounds; ++round) {
    g_now_us += 100'000;
    std::vector<Msg> to_b, to_a;
    to_b.swap(a.sent);
    to_a.swap(b.sent);
    for (auto& m : to_b) b.Receive(m.type, m.payload);
    for (auto& m : to_a) a.Receive(m.type, m.payload);
    a.sync.PollOnce();
    b.sync.PollOnce();
    if (to_a.empty() && to_b.empty() && a.sent.empty() && b.sent.empty() && round > 2) return;
  }
}

class TempDir {
 public:
  TempDir() {
    std::string tmpl = (fs::temp_directory_path() / "rdm-clip-XXXXXX").string();
    path_ = mkdtemp(tmpl.data());
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void WriteFile(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << data;
}

std::string ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string Pattern(size_t n, int seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131 + seed * 7 + i / 977) & 0xFF);
  return s;
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

// ---- with a peer of an older version (plain text messages only) ----------

TEST(ClipboardSync, HelloGoesFirst) {
  Side s;
  s.sync.Begin();
  s.sync.PollOnce();
  ASSERT_EQ(s.sent.size(), 1u);
  EXPECT_EQ(s.sent[0].type, kClipboardHelloMessageType);
  auto h = DecodeClipboardHello(s.sent[0].payload);
  ASSERT_TRUE(h);
  EXPECT_EQ(h->version, 1);
  EXPECT_EQ(h->max_file_bytes, 0u);  // no staging directory: no files taken
}

TEST(ClipboardSync, OlderPeerGetsPlainTextOnce) {
  Side s;
  s.sync.Begin();
  s.sync.PollOnce();
  s.clipboard->Copy("hello");
  s.sync.PollOnce();
  s.sync.PollOnce();
  EXPECT_EQ(s.SentTexts(), std::vector<std::string>{"hello"});
  EXPECT_EQ(s.Sent().size(), 1u);
}

TEST(ClipboardSync, WaitsForThePeersHelloBeforeFallingBack) {
  ClipboardSync::Options o = Fast();
  o.hello_wait_ms = 60'000;
  Side s(o);
  s.sync.Begin();
  s.sync.PollOnce();
  s.clipboard->Copy("hello");
  s.sync.PollOnce();
  EXPECT_TRUE(s.Sent().empty());  // the peer may yet say hello
  s.Receive(kClipboardHelloMessageType, EncodeClipboardHello({1, 0}));
  s.sync.PollOnce();
  ASSERT_EQ(s.SentOffers().size(), 1u);  // it did: version 1
  EXPECT_TRUE(s.SentTexts().empty());
}

TEST(ClipboardSync, WhatWasThereBeforeIsNotSentByDefault) {
  Side s;
  s.clipboard->Copy("old");
  s.sync.Begin();
  s.sync.PollOnce();
  EXPECT_TRUE(s.Sent().empty());
}

TEST(ClipboardSync, SendOnStartSendsWhatIsThere) {
  ClipboardSync::Options o = Fast();
  o.send_on_start = true;
  Side s(o);
  s.clipboard->Copy("copied before connecting");
  s.sync.Begin();
  s.sync.PollOnce();  // hello
  s.sync.PollOnce();
  EXPECT_EQ(s.SentTexts(), std::vector<std::string>{"copied before connecting"});
}

TEST(ClipboardSync, OlderPeersTextIsWritten) {
  Side s;
  s.sync.Begin();
  s.ReceiveText("from an older peer");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->text(), "from an older peer");
  s.sync.PollOnce();
  EXPECT_TRUE(s.Sent().empty());  // not echoed
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
  s.sync.PollOnce();  // hello
  s.sync.PollOnce();
  EXPECT_EQ(s.SentTexts(), std::vector<std::string>{"waiting"});
}

TEST(ClipboardSync, ConcealedAndEmptyAreNotSent) {
  Side s;
  s.sync.Begin();
  s.clipboard->Copy("hunter2", /*concealed=*/true);
  s.sync.PollOnce();
  s.clipboard->Copy("");
  s.sync.PollOnce();
  s.clipboard->CopyNothingShared();
  s.sync.PollOnce();
  EXPECT_TRUE(s.Sent().empty());
}

TEST(ClipboardSync, PlainTextSizeLimitBothWays) {
  ClipboardSync::Options o = Fast();
  o.max_text_bytes = 8;
  Side s(o);
  s.sync.Begin();
  s.clipboard->Copy("123456789");
  s.sync.PollOnce();
  EXPECT_TRUE(s.Sent().empty());
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

TEST(ClipboardSync, NewestRemoteTextWins) {
  Side s;
  s.sync.Begin();
  s.ReceiveText("first");
  s.ReceiveText("second");
  s.sync.PollOnce();
  EXPECT_EQ(s.clipboard->text(), "second");
}

TEST(ClipboardSync, OtherMessagesAreNotTaken) {
  EXPECT_FALSE(ClipboardSync::IsClipboardMessage(0x10002));
  EXPECT_TRUE(ClipboardSync::IsClipboardMessage(kClipboardTextMessageType));
  EXPECT_TRUE(ClipboardSync::IsClipboardMessage(kClipboardDataMessageType));
}

// ---- two sides of this version -------------------------------------------

TEST(ClipboardSync, TextGoesAcrossAndIsNotEchoed) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  a.clipboard->Copy("from a");
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->text(), "from a");
  EXPECT_EQ(b.clipboard->writes(), 1);
  EXPECT_EQ(a.clipboard->writes(), 0);

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

TEST(ClipboardSync, RichTextAndImagesGoAcross) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "bold"},
               {std::string(kMimeRtf), "{\\rtf1 \\b bold}"},
               {std::string(kMimeHtml), "<b>bold</b>"}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->content(), c);

  ClipboardContent img;
  img.formats = {{std::string(kMimePng), Pattern(700'000, 3)}};  // several chunks
  b.clipboard->CopyContent(img);
  Exchange(a, b);
  EXPECT_EQ(a.clipboard->content(), img);
}

TEST(ClipboardSync, LargeTextGoesAcrossInPieces) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  std::string big(6u << 20, 'x');  // more than one plain text message allows
  a.clipboard->Copy(big);
  Exchange(a, b, 1000);
  EXPECT_EQ(b.clipboard->text(), big);
}

TEST(ClipboardSync, UnknownFormatsAreNotSent) {
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{"application/x-secret", "zzz"}, {std::string(kMimeText), "plain"}};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->content(), ClipboardContent::Text("plain"));
}

TEST(ClipboardSync, ModesLimitTheDirection) {
  Side send_only(Mode(ClipboardMode::kSend));
  send_only.sync.Begin();
  send_only.ReceiveText("in");
  send_only.clipboard->Copy("out");
  send_only.sync.PollOnce();
  send_only.sync.PollOnce();
  EXPECT_EQ(send_only.clipboard->text(), "out");
  EXPECT_EQ(send_only.SentTexts(), std::vector<std::string>{"out"});

  Side receive_only(Mode(ClipboardMode::kReceive));
  receive_only.sync.Begin();
  receive_only.clipboard->Copy("out");
  receive_only.sync.PollOnce();
  EXPECT_TRUE(receive_only.Sent().empty());
  receive_only.ReceiveText("in");
  receive_only.sync.PollOnce();
  EXPECT_EQ(receive_only.clipboard->text(), "in");
  EXPECT_TRUE(receive_only.Sent().empty());

  // A version 1 offer to a side that does not receive is refused.
  Side a, send_only2(Mode(ClipboardMode::kSend));
  a.sync.Begin();
  send_only2.sync.Begin();
  a.clipboard->Copy("refused");
  Exchange(a, send_only2);
  EXPECT_EQ(send_only2.clipboard->writes(), 0);
}

TEST(ClipboardSync, ThreadedBothWays) {
  // Two syncs wired to each other through their send functions, on their
  // own threads.
  auto ca = std::make_shared<FakeClipboard>(), cb = std::make_shared<FakeClipboard>();
  std::atomic<int> offers{0};
  ClipboardSync::Options o;
  o.poll_ms = 10;
  ClipboardSync* pa = nullptr;
  ClipboardSync* pb = nullptr;
  ClipboardSync a(ca, [&](uint32_t type, std::string_view p) {
    if (type == kClipboardOfferMessageType) ++offers;
    pb->OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
    return true;
  }, o);
  ClipboardSync b(cb, [&](uint32_t type, std::string_view p) {
    if (type == kClipboardOfferMessageType) ++offers;
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
  EXPECT_TRUE(wait([&] { return cb->text() == "one"; }));
  cb->Copy("two");
  EXPECT_TRUE(wait([&] { return ca->text() == "two"; }));
  std::this_thread::sleep_for(100ms);  // no echo storm afterwards
  EXPECT_EQ(offers, 2);
  a.Stop();
  b.Stop();
}

// ---- files ---------------------------------------------------------------

class ClipboardFiles : public ::testing::Test {
 protected:
  // A sender paced as on a real link (8 Mbps to start), on the test clock.
  ClipboardSync::Options Paced(const fs::path& staging) {
    ClipboardSync::Options o = Opts(staging);
    o.pacer = TransferPacer::Options{};
    o.clock = [] { return g_now_us; };
    return o;
  }
  ClipboardSync::Options Opts(const fs::path& staging, uint64_t limit = kDefaultClipboardFileLimit) {
    ClipboardSync::Options o = Fast();
    o.staging_dir = staging.string();
    o.max_file_bytes = limit;
    o.on_status = [this](const ClipboardTransferStatus& s) { statuses.push_back(s); };
    return o;
  }
  TempDir src, sa, sb;
  std::vector<ClipboardTransferStatus> statuses;
};

TEST_F(ClipboardFiles, FilesAndFoldersArriveAndArePutOnTheClipboard) {
  WriteFile(src.path() / "report.txt", "hello report");
  WriteFile(src.path() / "photos" / "a.bin", Pattern(400'000, 1));
  WriteFile(src.path() / "photos" / "deep" / "b.bin", Pattern(5, 2));
  WriteFile(src.path() / "photos" / "empty.txt", "");
  fs::create_directories(src.path() / "photos" / "nothing here");
  fs::create_symlink("a.bin", src.path() / "photos" / "link");
  chmod((src.path() / "report.txt").c_str(), 0640);

  Side a(Opts(sa.path())), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "report.txt\nphotos"}, {std::string(kMimePng), "icon"}};
  c.files = {(src.path() / "report.txt").string(), (src.path() / "photos").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b, 1000);

  const ClipboardContent got = b.clipboard->content();
  ASSERT_EQ(got.files.size(), 2u);
  // Only the names as text: an icon is pointless next to the files.
  EXPECT_EQ(got.formats, (std::vector<ClipboardFormat>{{std::string(kMimeText), "report.txt\nphotos"}}));
  const fs::path report = got.files[0], photos = got.files[1];
  EXPECT_EQ(report.filename(), "report.txt");
  EXPECT_EQ(photos.filename(), "photos");
  EXPECT_EQ(report.parent_path().parent_path(), sb.path());  // in the staging directory
  EXPECT_EQ(ReadFile(report), "hello report");
  EXPECT_EQ(ReadFile(photos / "a.bin"), Pattern(400'000, 1));
  EXPECT_EQ(ReadFile(photos / "deep" / "b.bin"), Pattern(5, 2));
  EXPECT_TRUE(fs::exists(photos / "empty.txt"));
  EXPECT_EQ(fs::file_size(photos / "empty.txt"), 0u);
  EXPECT_TRUE(fs::is_directory(photos / "nothing here"));
  EXPECT_TRUE(fs::is_symlink(photos / "link"));
  EXPECT_EQ(fs::read_symlink(photos / "link"), "a.bin");
  struct stat st {};
  ASSERT_EQ(stat(report.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0640u);
  struct stat src_st {};
  ASSERT_EQ(stat((src.path() / "report.txt").c_str(), &src_st), 0);
  EXPECT_EQ(st.st_mtime, src_st.st_mtime);  // whole seconds travel

  // Progress both ways, ending in done.
  ASSERT_FALSE(statuses.empty());
  bool sent_done = false, received_done = false;
  for (const auto& s : statuses) {
    if (s.state != ClipboardTransferStatus::State::kDone) continue;
    EXPECT_EQ(s.files, 4u);
    EXPECT_EQ(s.bytes_done, s.bytes_total);
    EXPECT_EQ(s.name, "report.txt");
    (s.direction == ClipboardTransferStatus::Direction::kSending ? sent_done : received_done) = true;
  }
  EXPECT_TRUE(sent_done);
  EXPECT_TRUE(received_done);

  // Not sent back.
  a.sync.PollOnce();
  b.sync.PollOnce();
  EXPECT_TRUE(b.Sent().empty());
  EXPECT_EQ(a.clipboard->writes(), 0);
}

TEST_F(ClipboardFiles, OverTheLimitOnlyTheNamesGoAndBothSidesAreTold) {
  WriteFile(src.path() / "big.bin", std::string(2000, 'x'));
  Side a(Opts(sa.path(), 1000)), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "big.bin"}};
  c.files = {(src.path() / "big.bin").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->content(), ClipboardContent::Text("big.bin"));
  EXPECT_TRUE(fs::is_empty(sb.path()));
  int skipped = 0;
  for (const auto& s : statuses) {
    if (s.state != ClipboardTransferStatus::State::kSkipped) continue;
    ++skipped;
    EXPECT_EQ(s.detail, "too_large");
    EXPECT_EQ(s.skipped_bytes, 2000u);
    EXPECT_EQ(s.limit_bytes, 1000u);  // the sender's, smaller than the receiver's
    EXPECT_EQ(s.files, 1u);
  }
  EXPECT_EQ(skipped, 2);  // the sender and the receiver
}

TEST_F(ClipboardFiles, ThePeersLimitAppliesToo) {
  WriteFile(src.path() / "big.bin", std::string(2000, 'x'));
  Side a(Opts(sa.path())), b(Opts(sb.path(), 1000));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {(src.path() / "big.bin").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->writes(), 0);
  ASSERT_FALSE(statuses.empty());
  EXPECT_EQ(statuses.front().limit_bytes, 1000u);
}

TEST_F(ClipboardFiles, APeerWithoutStagingTakesNoFiles) {
  WriteFile(src.path() / "f.txt", "x");
  Side a(Opts(sa.path())), b;  // b: no staging directory
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "f.txt"}};
  c.files = {(src.path() / "f.txt").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_EQ(b.clipboard->content(), ClipboardContent::Text("f.txt"));
  ASSERT_FALSE(statuses.empty());
  EXPECT_EQ(statuses.front().detail, "not_accepted");
}

TEST_F(ClipboardFiles, ANewerCopySupersedesOneOnItsWay) {
  WriteFile(src.path() / "big.bin", Pattern(2'000'000, 5));
  Side a(Paced(sa.path())), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {(src.path() / "big.bin").string()};
  a.clipboard->CopyContent(c);
  // Only a few rounds: the transfer is under way, not finished.
  Exchange(a, b, 3);
  ASSERT_EQ(b.clipboard->writes(), 0);
  a.clipboard->Copy("never mind");
  Exchange(a, b, 1000);
  EXPECT_EQ(b.clipboard->content(), ClipboardContent::Text("never mind"));
  EXPECT_TRUE(fs::is_empty(sb.path()));  // the partial copy is gone
}

TEST_F(ClipboardFiles, ACopyOnTheReceivingSideCancelsTheIncomingOne) {
  WriteFile(src.path() / "big.bin", Pattern(2'000'000, 5));
  Side a(Paced(sa.path())), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {(src.path() / "big.bin").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b, 3);
  b.clipboard->Copy("mine is newer");
  Exchange(a, b, 1000);
  EXPECT_EQ(b.clipboard->text(), "mine is newer");
  EXPECT_EQ(a.clipboard->text(), "mine is newer");
  EXPECT_TRUE(fs::is_empty(sb.path()));
}

TEST_F(ClipboardFiles, OlderReceivedCopiesAreDeleted) {
  WriteFile(src.path() / "one.txt", "1");
  WriteFile(src.path() / "two.txt", "2");
  Side a(Opts(sa.path())), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {(src.path() / "one.txt").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  c.files = {(src.path() / "two.txt").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  ASSERT_EQ(b.clipboard->content().files.size(), 1u);
  EXPECT_EQ(ReadFile(b.clipboard->content().files[0]), "2");
  EXPECT_EQ(std::distance(fs::directory_iterator(sb.path()), fs::directory_iterator()), 1);
}

TEST_F(ClipboardFiles, StartingAgainKeepsOnlyWhatIsOnTheClipboard) {
  fs::create_directories(sb.path() / "1-old");
  fs::create_directories(sb.path() / "2-current" / "x");
  auto clip = std::make_shared<FakeClipboard>();
  ClipboardContent c;
  c.files = {(sb.path() / "2-current" / "x").string()};
  clip->CopyContent(c);
  ClipboardSync s(clip, [](uint32_t, std::string_view) { return true; }, Opts(sb.path()));
  s.Begin();
  EXPECT_FALSE(fs::exists(sb.path() / "1-old"));
  EXPECT_TRUE(fs::exists(sb.path() / "2-current" / "x"));
}

TEST_F(ClipboardFiles, UnsafeOffersAreRefused) {
  Side b(Opts(sb.path()));
  b.sync.Begin();
  b.sync.PollOnce();
  b.Receive(kClipboardHelloMessageType, EncodeClipboardHello({1, 0}));
  for (const char* path : {"../escape", "/etc/passwd", "a/../../x", "a\\..\\x/"}) {
    ClipboardOffer o;
    o.id = 1;
    o.entries = {File(path, 1)};
    b.Receive(kClipboardOfferMessageType, EncodeClipboardOffer(o));
    b.Receive(kClipboardDataMessageType, EncodeClipboardData({1, 0, 0}, "x"));
  }
  // A file inside a directory that is not listed first.
  ClipboardOffer o;
  o.id = 2;
  o.entries = {File("dir/f", 1), Dir("dir")};
  b.Receive(kClipboardOfferMessageType, EncodeClipboardOffer(o));
  b.sync.PollOnce();
  EXPECT_EQ(b.clipboard->writes(), 0);
  EXPECT_TRUE(fs::is_empty(sb.path()));
  EXPECT_FALSE(fs::exists(sb.path().parent_path() / "escape"));
}

TEST_F(ClipboardFiles, DataOutOfOrderDropsTheCopy) {
  Side b(Opts(sb.path()));
  b.sync.Begin();
  b.sync.PollOnce();
  b.Receive(kClipboardHelloMessageType, EncodeClipboardHello({1, 0}));
  ClipboardOffer o;
  o.id = 9;
  o.entries = {File("f", 4)};
  b.Receive(kClipboardOfferMessageType, EncodeClipboardOffer(o));
  b.Receive(kClipboardDataMessageType, EncodeClipboardData({9, 0, 2}, "zz"));  // skips bytes 0-1
  b.sync.PollOnce();
  EXPECT_EQ(b.clipboard->writes(), 0);
  EXPECT_TRUE(fs::is_empty(sb.path()));
  bool cancelled = false;
  for (const auto& m : b.sent) {
    if (m.type == kClipboardCancelMessageType) {
      auto c = DecodeClipboardCancel(m.payload);
      cancelled = c && c->id == 9 && !c->from_sender && c->reason == "error";
    }
  }
  EXPECT_TRUE(cancelled);
}

TEST_F(ClipboardFiles, AFileThatShrinksWhileSendingFailsCleanly) {
  WriteFile(src.path() / "f.bin", Pattern(1'000'000, 4));
  Side a(Paced(sa.path())), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {(src.path() / "f.bin").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b, 2);
  fs::resize_file(src.path() / "f.bin", 10);
  Exchange(a, b, 1000);
  EXPECT_EQ(b.clipboard->writes(), 0);
  EXPECT_TRUE(fs::is_empty(sb.path()));
  bool failed = false;
  for (const auto& s : statuses) failed |= s.state == ClipboardTransferStatus::State::kFailed;
  EXPECT_TRUE(failed);
}

TEST_F(ClipboardFiles, ThreadedWithPacing) {
  // Real threads and the real pacer: 3 MB arrive intact.
  WriteFile(src.path() / "f.bin", Pattern(3'000'000, 6));
  auto ca = std::make_shared<FakeClipboard>(), cb = std::make_shared<FakeClipboard>();
  ClipboardSync::Options oa = Opts(sa.path()), ob = Opts(sb.path());
  oa.pacer = ob.pacer = TransferPacer::Options{};
  oa.poll_ms = ob.poll_ms = 10;
  oa.on_status = ob.on_status = nullptr;
  ClipboardSync* pa = nullptr;
  ClipboardSync* pb = nullptr;
  ClipboardSync a(ca, [&](uint32_t type, std::string_view p) {
    pb->OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
    return true;
  }, oa);
  ClipboardSync b(cb, [&](uint32_t type, std::string_view p) {
    pa->OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
    return true;
  }, ob);
  pa = &a;
  pb = &b;
  a.Start();
  b.Start();
  ClipboardContent c;
  c.files = {(src.path() / "f.bin").string()};
  ca->CopyContent(c);
  bool arrived = false;
  for (int i = 0; i < 1000 && !arrived; ++i) {
    std::this_thread::sleep_for(10ms);
    arrived = cb->content().files.size() == 1;
  }
  ASSERT_TRUE(arrived);
  EXPECT_EQ(ReadFile(cb->content().files[0]), Pattern(3'000'000, 6));
  a.Stop();
  b.Stop();
}
