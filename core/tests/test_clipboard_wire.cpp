#include <gtest/gtest.h>

#include "rdmachine/clipboard_wire.h"

using namespace rdm;

namespace {

OfferEntry File(std::string path, uint64_t size, uint32_t mode = 0, int64_t mtime = 0) {
  return OfferEntry{std::move(path), OfferEntry::Kind::kFile, size, mode, mtime, {}};
}
OfferEntry Dir(std::string path, uint32_t mode = 0, int64_t mtime = 0) {
  return OfferEntry{std::move(path), OfferEntry::Kind::kDir, 0, mode, mtime, {}};
}
OfferEntry Link(std::string path, std::string target) {
  return OfferEntry{std::move(path), OfferEntry::Kind::kLink, 0, 0, 0, std::move(target)};
}
}  // namespace

TEST(ClipboardWire, HelloRoundTrip) {
  ClipboardHello h{1, 268435456};
  EXPECT_EQ(DecodeClipboardHello(EncodeClipboardHello(h)), h);
  // Missing limit: no files; later fields are ignored.
  auto h2 = DecodeClipboardHello(R"({"version":2,"future":true})");
  ASSERT_TRUE(h2);
  EXPECT_EQ(h2->version, 2);
  EXPECT_EQ(h2->max_file_bytes, 0u);
  EXPECT_FALSE(DecodeClipboardHello("{}"));
  EXPECT_FALSE(DecodeClipboardHello("not json"));
}

TEST(ClipboardWire, OfferRoundTrip) {
  ClipboardOffer o;
  o.id = 42;
  o.formats = {{"text/plain", 5}, {"image/png", 1000}};
  o.entries = {Dir("dir", 0755, 1700000000),
               File("dir/a.txt", 12, 0644, 1700000001),
               Link("dir/link", "a.txt"),
               File("文件 ✓.txt", 0)};
  o.skipped_files = 0;
  auto back = DecodeClipboardOffer(EncodeClipboardOffer(o));
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, o);
  EXPECT_EQ(o.ItemSizes(), (std::vector<uint64_t>{5, 1000, 12, 0}));
  EXPECT_EQ(o.TotalBytes(), 1017u);
  EXPECT_EQ(o.FileBytes(), 12u);
  EXPECT_EQ(o.FileCount(), 2u);
}

TEST(ClipboardWire, SkippedRoundTrip) {
  ClipboardOffer o;
  o.id = 1;
  o.formats = {{"text/plain", 3}};
  o.skipped_files = 3;
  o.skipped_bytes = 2'000'000'000;
  o.skipped_reason = "too_large";
  o.skipped_limit = 256u << 20;
  EXPECT_EQ(DecodeClipboardOffer(EncodeClipboardOffer(o)), o);
}

TEST(ClipboardWire, SafePaths) {
  for (const char* ok : {"a", "a/b", "a b/c.txt", "文件/✓", ".hidden", "a..b", "..."}) {
    EXPECT_TRUE(IsSafeRelativePath(ok)) << ok;
  }
  for (const char* bad : {"", "/a", "a/", "a//b", "./a", "a/./b", "..", "../a", "a/..", "a/../b", "a\nb"}) {
    EXPECT_FALSE(IsSafeRelativePath(bad)) << bad;
  }
  EXPECT_FALSE(IsSafeRelativePath(std::string("a\0b", 3)));
  EXPECT_FALSE(IsSafeRelativePath("bad\xFF"));
  EXPECT_FALSE(IsSafeRelativePath(std::string(256, 'n')));
  EXPECT_TRUE(IsSafeRelativePath(std::string(255, 'n')));
}

TEST(ClipboardWire, OffersMustListParentsFirstAndOnce) {
  ClipboardOffer o;
  o.entries = {File("d/f", 1), Dir("d")};
  EXPECT_FALSE(ValidateOffer(o));
  o.entries = {Dir("d"), File("d/f", 1)};
  EXPECT_TRUE(ValidateOffer(o));
  o.entries.push_back(File("d/f", 1));
  EXPECT_FALSE(ValidateOffer(o));
  // A file is no parent.
  o.entries = {File("f", 1), File("f/g", 1)};
  EXPECT_FALSE(ValidateOffer(o));
  // Nor a link.
  o.entries = {Link("l", "/etc"), File("l/passwd", 1)};
  EXPECT_FALSE(ValidateOffer(o));
  o.entries = {File("f", 1, 04755)};  // setuid
  EXPECT_FALSE(ValidateOffer(o));
}

TEST(ClipboardWire, MalformedOffersAreRefused) {
  EXPECT_FALSE(DecodeClipboardOffer("[]"));
  EXPECT_FALSE(DecodeClipboardOffer(R"({"formats":[]})"));  // no id
  EXPECT_FALSE(DecodeClipboardOffer(R"({"id":1,"entries":[{"path":"a","kind":"socket"}]})"));
  EXPECT_FALSE(DecodeClipboardOffer(R"({"id":1,"entries":[{"path":"a","kind":"file"}]})"));  // no size
  EXPECT_FALSE(DecodeClipboardOffer(R"({"id":1,"entries":[{"path":"../a","kind":"file","size":1}]})"));
  EXPECT_FALSE(DecodeClipboardOffer(R"({"id":-1})"));
  EXPECT_TRUE(DecodeClipboardOffer(R"({"id":1})"));
}

TEST(ClipboardWire, DataAckCancel) {
  std::string_view bytes;
  const std::string d = EncodeClipboardData({0x1122334455667788, 7, 0x100000000}, "payload");
  EXPECT_EQ(d.size(), kClipboardDataHeaderSize + 7);
  auto h = DecodeClipboardData(d, &bytes);
  ASSERT_TRUE(h);
  EXPECT_EQ(h->id, 0x1122334455667788u);
  EXPECT_EQ(h->item, 7u);
  EXPECT_EQ(h->offset, 0x100000000u);
  EXPECT_EQ(bytes, "payload");
  EXPECT_FALSE(DecodeClipboardData("short", &bytes));

  ClipboardAck a{5, 123456789012};
  EXPECT_EQ(DecodeClipboardAck(EncodeClipboardAck(a)), a);
  EXPECT_FALSE(DecodeClipboardAck("x"));

  ClipboardCancel c{9, true, "superseded"};
  EXPECT_EQ(DecodeClipboardCancel(EncodeClipboardCancel(c)), c);
  EXPECT_FALSE(DecodeClipboardCancel(R"({"id":1})"));
}
