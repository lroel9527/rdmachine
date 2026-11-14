#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "fake_clipboard.h"
#include "rdmachine/clipboard_sync.h"
#include "rdmachine/file_transfer.h"

using namespace rdm;

namespace {

std::string Pattern(size_t n, int seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131 + seed * 7 + i / 977) & 0xFF);
  return s;
}

void WriteFile(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary);
  f << data;
}

std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The one session folder FileTransfer made under a staging folder.
std::string SessionDir(const std::string& staging) {
  std::string out;
  if (DIR* d = opendir(staging.c_str())) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == 's') out = staging + "/" + e->d_name;
    }
    closedir(d);
  }
  return out;
}

struct TempDir {
  TempDir() {
    char tmpl[] = "/tmp/rdm-files-XXXXXX";
    path = mkdtemp(tmpl);
  }
  ~TempDir() { std::system(("rm -rf '" + path + "'").c_str()); }
  std::string path;
};

// Where the peer's files would appear: records what FileTransfer tells it.
struct FakeTarget : FileTarget {
  void SetRequester(FileRequester* r) override { requester = r; }
  std::optional<std::vector<std::string>> Publish(uint64_t announce, const std::vector<FileEntry>& items) override {
    if (!can_publish) return std::nullopt;
    published = announce;
    published_items = items;
    std::vector<std::string> out;
    for (const auto& e : items) out.push_back("/placeholders/" + e.path);
    return out;
  }
  void Withdraw(uint64_t announce) override { withdrawn.push_back(announce); }
  void OnList(uint64_t, uint32_t, const std::vector<FileEntry>& entries, const std::string& error) override {
    listed = entries;
    list_error = error;
    ++lists;
  }
  void OnProgress(uint64_t, uint32_t, const std::string&, uint64_t) override { ++progress; }
  void OnFileStarted(uint64_t, uint32_t, const FileEntry& e, const std::string&) override { started.push_back(e.path); }
  void OnFileReady(uint64_t, uint32_t, const FileEntry& e, const std::string& local) override {
    ready.emplace_back(e, local);
  }
  void OnItemDone(uint64_t, uint32_t, const std::string& error) override {
    done.push_back(error);
  }

  bool can_publish = true;
  FileRequester* requester = nullptr;
  uint64_t published = 0;
  std::vector<FileEntry> published_items;
  std::vector<uint64_t> withdrawn;
  std::vector<FileEntry> listed;
  std::string list_error;
  int lists = 0, progress = 0;
  std::vector<std::string> started;
  std::vector<std::pair<FileEntry, std::string>> ready;
  std::vector<std::string> done;
};

struct Msg {
  uint32_t type;
  std::string payload;
};

// The tests' clock: each round moves it on 10 ms.
uint64_t g_now_us = 1'000'000;

ClipboardSync::Options Fast(std::shared_ptr<FileTarget> target = nullptr, std::string staging = "") {
  ClipboardSync::Options o;
  o.hello_wait_ms = 0;
  o.pacer.start_bps = 1e9;
  o.clock = [] { return g_now_us; };
  o.file_target = std::move(target);
  o.file_staging_dir = std::move(staging);
  if (getenv("RDM_TEST_LOG")) o.log = [](const std::string& l) { fprintf(stderr, "%s\n", l.c_str()); };
  return o;
}

struct Side {
  explicit Side(ClipboardSync::Options o = Fast())
      : sync(clipboard, [this](uint32_t type, std::string_view p) { return Send(type, p); }, o) {}
  bool Send(uint32_t type, std::string_view p) {
    sent.push_back({type, std::string(p)});
    log.push_back({type, std::string(p)});
    return true;
  }
  void Receive(uint32_t type, const std::string& p) {
    sync.OnMessage(type, reinterpret_cast<const uint8_t*>(p.data()), p.size());
  }
  uint64_t Count(uint32_t type) const {
    uint64_t n = 0;
    for (const auto& m : log) n += m.type == type;
    return n;
  }
  std::shared_ptr<FakeClipboard> clipboard = std::make_shared<FakeClipboard>();
  std::vector<Msg> sent, log;
  ClipboardSync sync;
};

bool Round(Side& a, Side& b) {
  g_now_us += 10'000;
  std::vector<Msg> to_b, to_a;
  to_b.swap(a.sent);
  to_a.swap(b.sent);
  for (auto& m : to_b) b.Receive(m.type, m.payload);
  for (auto& m : to_a) a.Receive(m.type, m.payload);
  a.sync.PollOnce();
  b.sync.PollOnce();
  return !to_a.empty() || !to_b.empty();
}

void Exchange(Side& a, Side& b, int rounds = 2000) {
  for (int round = 0; round < rounds; ++round) {
    if (!Round(a, b) && a.sent.empty() && b.sent.empty() && round > 2) return;
  }
}

// A tree to copy: folder/{a.txt, sub/b.bin, empty/, link -> a.txt}, single.txt.
struct Tree {
  TempDir dir;
  std::string folder, single, a, b;
  Tree() {
    folder = dir.path + "/folder";
    single = dir.path + "/single.txt";
    mkdir(folder.c_str(), 0755);
    mkdir((folder + "/sub").c_str(), 0750);
    mkdir((folder + "/empty").c_str(), 0700);
    a = Pattern(300'000, 1);
    b = Pattern(1'500'000, 2);
    WriteFile(folder + "/a.txt", a);
    WriteFile(folder + "/sub/b.bin", b);
    symlink("a.txt", (folder + "/link").c_str());
    WriteFile(single, "hello");
    chmod(single.c_str(), 0640);
  }
};

}  // namespace

TEST(FilePaths, SafeAndUnsafe) {
  EXPECT_TRUE(IsSafeRelativePath("a", false));
  EXPECT_TRUE(IsSafeRelativePath("a/b c/d.txt", false));
  EXPECT_TRUE(IsSafeRelativePath("", true));
  EXPECT_FALSE(IsSafeRelativePath("", false));
  EXPECT_FALSE(IsSafeRelativePath("/etc/passwd", false));
  EXPECT_FALSE(IsSafeRelativePath("a/../b", false));
  EXPECT_FALSE(IsSafeRelativePath("..", false));
  EXPECT_FALSE(IsSafeRelativePath("a//b", false));
  EXPECT_FALSE(IsSafeRelativePath("a/", false));
  EXPECT_FALSE(IsSafeRelativePath("./a", false));
  EXPECT_FALSE(IsSafeRelativePath(std::string("a\0b", 3), false));
  EXPECT_TRUE(IsSafeFileName("报告.pdf"));
  EXPECT_FALSE(IsSafeFileName("a/b"));
}

TEST(FileStream, RoundTripInOddPieces) {
  struct H : FileStreamParser::Handler {
    bool OnEntry(const FileEntry& e) override {
      entries.push_back(e);
      contents.emplace_back();
      return true;
    }
    bool OnContent(std::string_view b) override {
      contents.back().append(b);
      return true;
    }
    bool OnEntryEnd() override {
      ++ends;
      return true;
    }
    void OnEnd() override { ended = true; }
    std::vector<FileEntry> entries;
    std::vector<std::string> contents;
    int ends = 0;
    bool ended = false;
  } h;
  std::vector<FileEntry> in = {{"", FileKind::kDir, 0, 0755, 100, ""},
                               {"a.txt", FileKind::kFile, 5, 0644, 200, ""},
                               {"l", FileKind::kLink, 0, 0777, 300, "a.txt"},
                               {"e", FileKind::kFile, 0, 0600, 400, ""}};
  std::string stream = EncodeFileRecord(in[0]) + EncodeFileRecord(in[1]) + "hello" + EncodeFileRecord(in[2]) +
                       EncodeFileRecord(in[3]) + EncodeFileStreamEnd();
  FileStreamParser p(&h);
  for (size_t i = 0; i < stream.size(); i += 3) ASSERT_TRUE(p.Feed(std::string_view(stream).substr(i, 3))) << p.error();
  EXPECT_TRUE(p.ended());
  EXPECT_TRUE(h.ended);
  EXPECT_EQ(h.entries, in);
  EXPECT_EQ(h.contents[1], "hello");
  EXPECT_EQ(h.ends, 4);
  EXPECT_FALSE(p.Feed("x"));  // nothing after the end
}

TEST(FileStream, RefusesUnsafePaths) {
  struct H : FileStreamParser::Handler {
    bool OnEntry(const FileEntry&) override { return true; }
    bool OnContent(std::string_view) override { return true; }
    bool OnEntryEnd() override { return true; }
    void OnEnd() override {}
  } h;
  FileStreamParser p(&h);
  EXPECT_FALSE(p.Feed(EncodeFileRecord({"../evil", FileKind::kFile, 1, 0644, 0, ""})));
}

TEST(FileWalk, ListsTheTreeParentsFirst) {
  Tree t;
  std::vector<FileEntry> entries;
  std::string error;
  ASSERT_TRUE(WalkLocalTree(t.folder, 100, &entries, &error)) << error;
  std::vector<std::string> paths;
  for (const auto& e : entries) paths.push_back(e.path);
  EXPECT_EQ(paths, (std::vector<std::string>{"", "a.txt", "empty", "link", "sub", "sub/b.bin"}));
  EXPECT_EQ(entries[3].kind, FileKind::kLink);
  EXPECT_EQ(entries[3].link, "a.txt");
  EXPECT_EQ(entries[5].size, t.b.size());
  EXPECT_FALSE(WalkLocalTree(t.folder, 3, &entries, &error));
}

TEST(FileTransfer, ACopyAnnouncesNamesAndSizesOnly) {
  Tree t;
  TempDir staging;
  auto target = std::make_shared<FakeTarget>();
  Side a, b(Fast(target, staging.path));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c = ClipboardContent::Text("folder\nsingle.txt");
  c.files = {t.folder, t.single};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  ASSERT_EQ(target->published_items.size(), 2u);
  EXPECT_EQ(target->published_items[0].path, "folder");
  EXPECT_EQ(target->published_items[0].kind, FileKind::kDir);
  EXPECT_EQ(target->published_items[1].path, "single.txt");
  EXPECT_EQ(target->published_items[1].size, 5u);
  EXPECT_EQ(target->published_items[1].mode, 0640u);
  auto promise = b.clipboard->promise();
  ASSERT_TRUE(promise);
  EXPECT_EQ(promise->file_paths(), (std::vector<std::string>{"/placeholders/folder", "/placeholders/single.txt"}));
  EXPECT_EQ(promise->formats(), (std::vector<std::string>{"text/plain"}));
  EXPECT_EQ(a.Count(kFileDataMessageType), 0u);  // nothing until it is pasted
}

TEST(FileTransfer, PastingAFolderBringsTheWholeTree) {
  Tree t;
  TempDir staging;
  auto target = std::make_shared<FakeTarget>();
  Side a, b(Fast(target, staging.path));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {t.folder, t.single};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  ASSERT_TRUE(target->requester);

  target->requester->RequestFileList(target->published, 0);
  Exchange(a, b);
  ASSERT_EQ(target->lists, 1);
  EXPECT_EQ(target->list_error, "");
  EXPECT_EQ(target->listed.size(), 6u);

  target->requester->RequestFileFetch(target->published, 0);
  target->requester->RequestFileFetch(target->published, 0);  // once is enough
  Exchange(a, b);
  ASSERT_EQ(target->done, std::vector<std::string>{""});
  EXPECT_EQ(a.Count(kFileFetchMessageType), 0u);
  EXPECT_EQ(b.Count(kFileFetchMessageType), 1u);
  const std::string session = SessionDir(staging.path);
  const std::string got = session + "/" + std::to_string(target->published) + "/0/folder";
  EXPECT_EQ(ReadFile(got + "/a.txt"), t.a);
  EXPECT_EQ(ReadFile(got + "/sub/b.bin"), t.b);
  char link[64] = {};
  ASSERT_GT(readlink((got + "/link").c_str(), link, sizeof link - 1), 0);
  EXPECT_STREQ(link, "a.txt");
  struct stat st;
  ASSERT_EQ(stat((got + "/empty").c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  ASSERT_EQ(stat((got + "/sub").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0750u);
  // Every file, link and folder was reported, the item itself among them;
  // the regular files also when they began to arrive.
  EXPECT_EQ(target->ready.size(), 6u);
  EXPECT_EQ(target->started, (std::vector<std::string>{"a.txt", "sub/b.bin"}));
  EXPECT_GT(target->progress, 0);

  target->requester->RequestFileFetch(target->published, 1);
  Exchange(a, b);
  EXPECT_EQ(ReadFile(session + "/" + std::to_string(target->published) + "/1/single.txt"), "hello");
  ASSERT_EQ(stat((session + "/" + std::to_string(target->published) + "/1/single.txt").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0640u);
}

TEST(FileTransfer, ANewCopyWithdrawsAndCleansUp) {
  Tree t;
  TempDir staging;
  auto target = std::make_shared<FakeTarget>();
  Side a, b(Fast(target, staging.path));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {t.single};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  const uint64_t first = target->published;
  target->requester->RequestFileFetch(first, 0);
  Exchange(a, b);
  const std::string dir = SessionDir(staging.path) + "/" + std::to_string(first);
  struct stat st;
  ASSERT_EQ(stat(dir.c_str(), &st), 0);
  b.clipboard->Copy("something else");  // copied on the pasting side
  Exchange(a, b);
  EXPECT_EQ(target->withdrawn, std::vector<uint64_t>{first});
  EXPECT_NE(stat(dir.c_str(), &st), 0);
  // An old announcement's item is gone.
  target->requester->RequestFileFetch(first, 0);
  Exchange(a, b);
  EXPECT_EQ(target->done.back(), "gone");
}

TEST(FileTransfer, AVersion3PeerGetsNamesOnly) {
  Tree t;
  Side a;
  a.sync.Begin();
  a.sync.PollOnce();
  a.Receive(kClipboardHelloMessageType, EncodeClipboardHello({3}));
  ClipboardContent c = ClipboardContent::Text("single.txt");
  c.files = {t.single};
  a.clipboard->CopyContent(c);
  a.sync.PollOnce();
  bool announced = false;
  for (const auto& m : a.log) {
    if (m.type != kClipboardAnnounceMessageType) continue;
    auto an = DecodeClipboardAnnounce(m.payload);
    ASSERT_TRUE(an);
    EXPECT_TRUE(an->files.empty());
    EXPECT_EQ(an->formats, std::vector<std::string>{"text/plain"});
    announced = true;
  }
  EXPECT_TRUE(announced);
}

TEST(FileTransfer, WithoutATargetFilesArePastedAsNames) {
  Tree t;
  Side a, b;
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c = ClipboardContent::Text("single.txt");
  c.files = {t.single};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  auto promise = b.clipboard->promise();
  ASSERT_TRUE(promise);
  EXPECT_TRUE(promise->file_paths().empty());
  EXPECT_EQ(promise->formats(), std::vector<std::string>{"text/plain"});
}

TEST(FileTransfer, AFileThatCannotBeReadFailsTheFetch) {
  Tree t;
  TempDir staging;
  auto target = std::make_shared<FakeTarget>();
  Side a, b(Fast(target, staging.path));
  a.sync.Begin();
  b.sync.Begin();
  ClipboardContent c;
  c.files = {t.single};
  a.clipboard->CopyContent(c);
  Exchange(a, b);
  unlink(t.single.c_str());  // gone before it is pasted
  target->requester->RequestFileFetch(target->published, 0);
  Exchange(a, b);
  ASSERT_EQ(target->done.size(), 1u);
  EXPECT_EQ(target->done[0], "error");
}
