#include <gtest/gtest.h>

#include "rdmachine/frame.h"

namespace {

TEST(Frame, EncodesBigEndianLength) {
  auto f = rdm::EncodeFrame("{}");
  ASSERT_TRUE(f);
  EXPECT_EQ(*f, (std::vector<uint8_t>{0, 0, 0, 2, '{', '}'}));
}

TEST(Frame, RejectsEmptyAndOversizedPayloads) {
  EXPECT_FALSE(rdm::EncodeFrame(""));
  EXPECT_TRUE(rdm::EncodeFrame(std::string(rdm::kMaxFramePayload, 'a')));
  EXPECT_FALSE(rdm::EncodeFrame(std::string(rdm::kMaxFramePayload + 1, 'a')));
}

TEST(Frame, DecodesFramesSplitAcrossReads) {
  std::vector<uint8_t> stream;
  for (const char* p : {"{\"a\":1}", "x", "{\"bb\":22}"}) {
    auto f = rdm::EncodeFrame(p);
    stream.insert(stream.end(), f->begin(), f->end());
  }
  rdm::FrameDecoder d;
  // One byte at a time: the worst case for TLS record boundaries.
  for (uint8_t b : stream) ASSERT_TRUE(d.Feed(std::span(&b, 1)));
  EXPECT_EQ(d.Next(), "{\"a\":1}");
  EXPECT_EQ(d.Next(), "x");
  EXPECT_EQ(d.Next(), "{\"bb\":22}");
  EXPECT_FALSE(d.Next());
  EXPECT_EQ(d.buffered(), 0u);
}

TEST(Frame, KeepsPartialFrameBuffered) {
  auto f = rdm::EncodeFrame("hello");
  rdm::FrameDecoder d;
  ASSERT_TRUE(d.Feed(std::span(f->data(), f->size() - 1)));
  EXPECT_FALSE(d.Next());
  EXPECT_EQ(d.buffered(), f->size() - 1);
  ASSERT_TRUE(d.Feed(std::span(f->data() + f->size() - 1, 1)));
  EXPECT_EQ(d.Next(), "hello");
}

TEST(Frame, FailsOnOversizedOrZeroLengthHeader) {
  const uint32_t too_big = rdm::kMaxFramePayload + 1;
  const uint8_t big[] = {uint8_t(too_big >> 24), uint8_t(too_big >> 16), uint8_t(too_big >> 8), uint8_t(too_big)};
  rdm::FrameDecoder d;
  EXPECT_FALSE(d.Feed(big));
  EXPECT_TRUE(d.failed());
  const uint8_t more[] = {0, 0, 0, 1, 'x'};
  EXPECT_FALSE(d.Feed(more));  // stays failed

  rdm::FrameDecoder z;
  const uint8_t zero[] = {0, 0, 0, 0};
  EXPECT_FALSE(z.Feed(zero));
}

}  // namespace
