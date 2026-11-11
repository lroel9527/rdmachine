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

std::optional<std::vector<std::string>> PasteFiles(Side& at, Side& other) {
  auto p = at.clipboard->promise();
  if (!p) return std::nullopt;
  return p->FetchFiles([&] { Round(at, other); }, 5000);
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
  EXPECT_EQ(h->version, 2);
  EXPECT_EQ(h->max_file_bytes, 0u);  // no staging directory: no files taken
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
  EXPECT_EQ(p->files(), 0u);
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
  b.Receive(kClipboardHelloMessageType, EncodeClipboardHello({2, 0}));
  ClipboardOffer o;
  o.id = 5;
  o.formats = {{"text/plain", 3}};
  b.Receive(kClipboardOfferMessageType, EncodeClipboardOffer(o));
  b.Receive(kClipboardDataMessageType, EncodeClipboardData({5, 0, 0}, "abc"));
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

TEST(ClipboardSync, AVersion1PeerIsAnOlderPeer) {
  Side s(Server());
  s.sync.Begin();
  s.sync.PollOnce();
  s.Receive(kClipboardHelloMessageType, EncodeClipboardHello({1, 1 << 20}));
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
  s.Receive(kClipboardHelloMessageType, EncodeClipboardHello({2, 0}));
  s.sync.PollOnce();
  EXPECT_EQ(s.Logged(kClipboardAnnounceMessageType).size(), 1u);  // it did: version 2
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

// ---- files ---------------------------------------------------------------

class ClipboardFiles : public ::testing::Test {
 protected:
  ClipboardSync::Options Opts(const fs::path& staging, uint64_t limit = kDefaultClipboardFileLimit) {
    ClipboardSync::Options o = Fast();
    o.staging_dir = staging.string();
    o.max_file_bytes = limit;
    o.on_status = [this](const ClipboardTransferStatus& s) { statuses.push_back(s); };
    return o;
  }
  // A sender paced as on a real link (8 Mbps to start), on the test clock.
  ClipboardSync::Options Paced(const fs::path& staging) {
    ClipboardSync::Options o = Opts(staging);
    o.pacer = TransferPacer::Options{};
    o.clock = [] { return g_now_us; };
    return o;
  }
  TempDir src, sa, sb;
  std::vector<ClipboardTransferStatus> statuses;
};

TEST_F(ClipboardFiles, CopiedFilesStayUntilPasted) {
  WriteFile(src.path() / "private.txt", "do not send");
  Side a(Opts(sa.path())), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "private.txt"}};
  c.files = {(src.path() / "private.txt").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  // Announced: one file, nothing more (not even its name or size).
  auto ann = DecodeClipboardAnnounce(a.Logged(kClipboardAnnounceMessageType).at(0));
  ASSERT_TRUE(ann);
  EXPECT_EQ(ann->files, 1u);
  EXPECT_TRUE(a.Logged(kClipboardOfferMessageType).empty());
  for (const auto& m : a.log) EXPECT_EQ(m.payload.find("private"), std::string::npos);
  EXPECT_TRUE(fs::is_empty(sb.path()));
  ASSERT_TRUE(b.clipboard->promise());
  EXPECT_EQ(b.clipboard->promise()->files(), 1u);
  EXPECT_TRUE(statuses.empty());  // and no notice anywhere
}

TEST_F(ClipboardFiles, PastingFetchesFilesAndFolders) {
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
  Exchange(a, b);
  auto p = b.clipboard->promise();
  ASSERT_TRUE(p);
  // Only the names as text: an icon is pointless next to the files.
  EXPECT_EQ(p->formats(), (std::vector<std::string>{"text/plain"}));
  EXPECT_EQ(p->files(), 2u);

  auto got = PasteFiles(b, a);
  ASSERT_TRUE(got);
  ASSERT_EQ(got->size(), 2u);
  const fs::path report = (*got)[0], photos = (*got)[1];
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
  EXPECT_EQ(Paste(b, a, kMimeText), "report.txt\nphotos");

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
  // Pasting again uses what is here.
  const size_t requests = b.Logged(kClipboardRequestMessageType).size();
  EXPECT_EQ(PasteFiles(b, a), got);
  EXPECT_EQ(b.Logged(kClipboardRequestMessageType).size(), requests);
}

TEST_F(ClipboardFiles, OverTheLimitThePasteGetsNothingAndBothSidesAreTold) {
  WriteFile(src.path() / "big.bin", std::string(2000, 'x'));
  Side a(Opts(sa.path(), 1000)), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "big.bin"}};
  c.files = {(src.path() / "big.bin").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  EXPECT_TRUE(statuses.empty());  // nothing said before a paste
  EXPECT_FALSE(PasteFiles(b, a));
  EXPECT_EQ(Paste(b, a, kMimeText), "big.bin");  // the name still pastes
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
  EXPECT_FALSE(PasteFiles(b, a));
  ASSERT_FALSE(statuses.empty());
  EXPECT_EQ(statuses.front().limit_bytes, 1000u);
}

TEST_F(ClipboardFiles, APeerWithoutStagingIsPromisedOnlyTheNames) {
  WriteFile(src.path() / "f.txt", "x");
  Side a(Opts(sa.path())), b;  // b: no staging directory
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.formats = {{std::string(kMimeText), "f.txt"}};
  c.files = {(src.path() / "f.txt").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  auto p = b.clipboard->promise();
  ASSERT_TRUE(p);
  EXPECT_EQ(p->files(), 0u);
  EXPECT_EQ(Paste(b, a, kMimeText), "f.txt");
}

TEST_F(ClipboardFiles, ANewerCopyStopsAFetchOnItsWay) {
  WriteFile(src.path() / "big.bin", Pattern(2'000'000, 5));
  Side a(Paced(sa.path())), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {(src.path() / "big.bin").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  auto p = b.clipboard->promise();
  int rounds = 0;
  // Three rounds into the fetch, a copies something else.
  auto got = p->FetchFiles([&] {
    if (++rounds == 3) a.clipboard->Copy("never mind");
    Round(b, a);
  }, 5000);
  EXPECT_FALSE(got);
  Exchange(a, b);
  EXPECT_EQ(Paste(b, a, kMimeText), "never mind");
  EXPECT_TRUE(fs::is_empty(sb.path()));  // the partial copy is gone
}

TEST_F(ClipboardFiles, OlderFetchedCopiesAreDeleted) {
  WriteFile(src.path() / "one.txt", "1");
  WriteFile(src.path() / "two.txt", "2");
  Side a(Opts(sa.path())), b(Opts(sb.path()));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {(src.path() / "one.txt").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  ASSERT_TRUE(PasteFiles(b, a));
  c.files = {(src.path() / "two.txt").string()};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  auto got = PasteFiles(b, a);
  ASSERT_TRUE(got);
  EXPECT_EQ(ReadFile((*got)[0]), "2");
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

// A peer that answers b's request for files with `offer` (and `data`).
struct FakeSender {
  Side& b;
  void Announce() {
    b.Receive(kClipboardHelloMessageType, EncodeClipboardHello({2, 0}));
    b.Receive(kClipboardAnnounceMessageType, EncodeClipboardAnnounce({1, {}, 1}));
    b.sync.PollOnce();
  }
  // Waits for the request b sends when its promise is asked.
  uint64_t RequestId() {
    for (const auto& m : b.log) {
      if (m.type == kClipboardRequestMessageType) return DecodeClipboardRequest(m.payload)->id;
    }
    return 0;
  }
};

TEST_F(ClipboardFiles, UnsafeAnswersAreRefused) {
  for (const char* path : {"../escape", "/etc/passwd", "a/../../x"}) {
    Side b(Opts(sb.path()));
    b.sync.Begin();
    b.sync.PollOnce();
    FakeSender peer{b};
    peer.Announce();
    auto p = b.clipboard->promise();
    ASSERT_TRUE(p);
    bool answered = false;
    auto got = p->FetchFiles([&] {
      b.sync.PollOnce();
      if (!answered && peer.RequestId() != 0) {
        answered = true;
        ClipboardOffer o;
        o.id = 9;
        o.request = peer.RequestId();
        o.entries = {File(path, 1)};
        // Encoded by hand: EncodeClipboardOffer would produce it, the decoder refuses it.
        b.Receive(kClipboardOfferMessageType, EncodeClipboardOffer(o));
        b.Receive(kClipboardDataMessageType, EncodeClipboardData({9, 0, 0}, "x"));
      }
    }, 300);
    EXPECT_FALSE(got) << path;
    EXPECT_TRUE(fs::is_empty(sb.path())) << path;
  }
  EXPECT_FALSE(fs::exists(sb.path().parent_path() / "escape"));
}

TEST_F(ClipboardFiles, DataOutOfOrderDropsTheFetch) {
  Side b(Opts(sb.path()));
  b.sync.Begin();
  b.sync.PollOnce();
  FakeSender peer{b};
  peer.Announce();
  bool answered = false;
  auto got = b.clipboard->promise()->FetchFiles([&] {
    b.sync.PollOnce();
    if (!answered && peer.RequestId() != 0) {
      answered = true;
      ClipboardOffer o;
      o.id = 9;
      o.request = peer.RequestId();
      o.entries = {File("f", 4)};
      b.Receive(kClipboardOfferMessageType, EncodeClipboardOffer(o));
      b.Receive(kClipboardDataMessageType, EncodeClipboardData({9, 0, 2}, "zz"));  // skips bytes 0-1
    }
  }, 2000);
  EXPECT_FALSE(got);
  EXPECT_TRUE(fs::is_empty(sb.path()));
  bool cancelled = false;
  for (const auto& m : b.log) {
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
  Exchange(a, b);
  int rounds = 0;
  auto got = b.clipboard->promise()->FetchFiles([&] {
    if (++rounds == 3) fs::resize_file(src.path() / "f.bin", 10);
    Round(b, a);
  }, 5000);
  EXPECT_FALSE(got);
  EXPECT_TRUE(fs::is_empty(sb.path()));
  bool failed = false;
  for (const auto& s : statuses) failed |= s.state == ClipboardTransferStatus::State::kFailed;
  EXPECT_TRUE(failed);
}

TEST_F(ClipboardFiles, ThreadedWithPacing) {
  // Real threads and the real pacer: 3 MB arrive intact when pasted.
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
  for (int i = 0; i < 300 && !cb->promise(); ++i) std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(cb->promise());
  int pumps = 0;
  auto got = cb->promise()->FetchFiles([&] { ++pumps; });
  ASSERT_TRUE(got);
  EXPECT_EQ(ReadFile((*got)[0]), Pattern(3'000'000, 6));
  EXPECT_GT(pumps, 0);  // the waiting side kept pumping
  a.Stop();
  b.Stop();
}
