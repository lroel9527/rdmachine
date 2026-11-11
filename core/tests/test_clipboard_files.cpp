#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "rdmachine/clipboard_files.h"

using namespace rdm;
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

class Tmp : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tmpl = (fs::temp_directory_path() / "rdm-files-XXXXXX").string();
    dir_ = mkdtemp(tmpl.data());
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  void Write(const fs::path& rel, const std::string& data) {
    fs::create_directories((dir_ / rel).parent_path());
    std::ofstream(dir_ / rel, std::ios::binary) << data;
  }
  fs::path dir_;
};

}  // namespace

TEST_F(Tmp, ListsFoldersInOrderWithParentsFirst) {
  Write("src/b/z.txt", "zz");
  Write("src/b/a.txt", "a");
  Write("src/top.txt", "top");
  ClipboardOffer o;
  std::vector<std::string> sources;
  ASSERT_TRUE(ListFiles({(dir_ / "src/b").string(), (dir_ / "src/top.txt").string()}, {1000, 100}, &o, &sources,
                        nullptr));
  ASSERT_EQ(o.entries.size(), 4u);
  EXPECT_EQ(o.entries[0].path, "b");
  EXPECT_EQ(o.entries[0].kind, OfferEntry::Kind::kDir);
  EXPECT_EQ(o.entries[1].path, "b/a.txt");
  EXPECT_EQ(o.entries[2].path, "b/z.txt");
  EXPECT_EQ(o.entries[3].path, "top.txt");
  EXPECT_EQ(sources, (std::vector<std::string>{(dir_ / "src/b/a.txt").string(), (dir_ / "src/b/z.txt").string(),
                                               (dir_ / "src/top.txt").string()}));
  EXPECT_TRUE(ValidateOffer(o));
}

TEST_F(Tmp, RootsOfTheSameNameAreNumbered) {
  Write("one/same.txt", "1");
  Write("two/same.txt", "2");
  ClipboardOffer o;
  std::vector<std::string> sources;
  ASSERT_TRUE(ListFiles({(dir_ / "one/same.txt").string(), (dir_ / "two/same.txt").string()}, {1000, 100}, &o,
                        &sources, nullptr));
  ASSERT_EQ(o.entries.size(), 2u);
  EXPECT_EQ(o.entries[0].path, "same.txt");
  EXPECT_EQ(o.entries[1].path, "same 2.txt");
}

TEST_F(Tmp, OverTheLimitNothingIsListedButTheTotalIsKnown) {
  Write("a.bin", std::string(600, 'a'));
  Write("b.bin", std::string(600, 'b'));
  ClipboardOffer o;
  std::vector<std::string> sources;
  ASSERT_TRUE(ListFiles({(dir_ / "a.bin").string(), (dir_ / "b.bin").string()}, {1000, 100}, &o, &sources, nullptr));
  EXPECT_TRUE(o.entries.empty());
  EXPECT_TRUE(sources.empty());
  EXPECT_EQ(o.skipped_files, 2u);
  EXPECT_EQ(o.skipped_bytes, 1200u);
  EXPECT_EQ(o.skipped_reason, "too_large");

  ClipboardOffer many;
  ASSERT_TRUE(ListFiles({(dir_ / "a.bin").string(), (dir_ / "b.bin").string()}, {10000, 1}, &many, &sources, nullptr));
  EXPECT_EQ(many.skipped_reason, "too_many");

  ClipboardOffer none;
  ASSERT_TRUE(ListFiles({(dir_ / "a.bin").string()}, {0, 100}, &none, &sources, nullptr));
  EXPECT_EQ(none.skipped_reason, "not_accepted");
}

TEST_F(Tmp, MissingRootsAreLeftOut) {
  Write("here.txt", "x");
  ClipboardOffer o;
  std::vector<std::string> sources;
  EXPECT_TRUE(ListFiles({(dir_ / "gone").string(), (dir_ / "here.txt").string()}, {1000, 100}, &o, &sources, nullptr));
  EXPECT_EQ(o.entries.size(), 1u);
  std::string error;
  EXPECT_FALSE(ListFiles({(dir_ / "gone").string()}, {1000, 100}, &o, &sources, &error));
  EXPECT_FALSE(error.empty());
}

TEST_F(Tmp, StagingIsPrivateAndRemovable) {
  ClipboardOffer o;
  o.entries = {Dir("d", 0555), File("d/f", 3, 0444)};
  std::string error;
  auto s = StagingDir::Create((dir_ / "staging").string(), o, &error);
  ASSERT_TRUE(s) << error;
  EXPECT_EQ(fs::status(dir_ / "staging").permissions() & fs::perms::all, fs::perms::owner_all);
  ASSERT_TRUE(s->Open(1, &error)) << error;
  ASSERT_TRUE(s->Append("abc", &error));
  ASSERT_TRUE(s->Close(&error));
  s->Finish();
  ASSERT_EQ(s->RootPaths().size(), 1u);
  EXPECT_EQ(fs::path(s->RootPaths()[0]).filename(), "d");
  EXPECT_EQ(StagingDirOf((dir_ / "staging").string(), s->RootPaths()[0] + "/f"), s->path());
  EXPECT_EQ(StagingDirOf((dir_ / "staging").string(), "/elsewhere/f"), "");
  // Read-only directories and files still go away.
  s->Remove();
  EXPECT_FALSE(fs::exists(s->path()));
}

TEST_F(Tmp, AFileCannotBeOpenedTwice) {
  ClipboardOffer o;
  o.entries = {File("f", 1)};
  std::string error;
  auto s = StagingDir::Create(dir_.string(), o, &error);
  ASSERT_TRUE(s);
  ASSERT_TRUE(s->Open(0, &error));
  ASSERT_TRUE(s->Close(&error));
  EXPECT_FALSE(s->Open(0, &error));  // O_EXCL
}
