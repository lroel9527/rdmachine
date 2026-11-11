#include <gtest/gtest.h>

#include "rdmachine/clipboard_wire.h"

using namespace rdm;

TEST(ClipboardWire, HelloRoundTrip) {
  ClipboardHello h{3};
  EXPECT_EQ(DecodeClipboardHello(EncodeClipboardHello(h)), h);
  // Later fields are ignored.
  auto h2 = DecodeClipboardHello(R"({"version":4,"future":true})");
  ASSERT_TRUE(h2);
  EXPECT_EQ(h2->version, 4);
  EXPECT_FALSE(DecodeClipboardHello("{}"));
  EXPECT_FALSE(DecodeClipboardHello("not json"));
}

TEST(ClipboardWire, AnnounceAndRequest) {
  ClipboardAnnounce a{3, {"text/plain", "image/png"}};
  EXPECT_EQ(DecodeClipboardAnnounce(EncodeClipboardAnnounce(a)), a);
  EXPECT_EQ(EncodeClipboardAnnounce(a), R"({"formats":["text/plain","image/png"],"id":3})");
  EXPECT_FALSE(DecodeClipboardAnnounce(R"({"formats":[]})"));  // no id
  EXPECT_FALSE(DecodeClipboardAnnounce(R"({"id":1,"formats":[7]})"));
  EXPECT_FALSE(DecodeClipboardAnnounce(R"({"id":1,"formats":[""]})"));

  ClipboardRequest r{3, 1, "text/plain"};
  EXPECT_EQ(DecodeClipboardRequest(EncodeClipboardRequest(r)), r);
  EXPECT_FALSE(DecodeClipboardRequest(R"({"announce":3,"id":1})"));  // no format
  EXPECT_FALSE(DecodeClipboardRequest(R"({"announce":3,"id":2,"files":true})"));  // version 2's file request
}

TEST(ClipboardWire, OfferRoundTrip) {
  ClipboardOffer o{4, 1, "image/png", 123456, ""};
  EXPECT_EQ(DecodeClipboardOffer(EncodeClipboardOffer(o)), o);
  ClipboardOffer gone{5, 2, "", 0, "gone"};
  EXPECT_EQ(DecodeClipboardOffer(EncodeClipboardOffer(gone)), gone);
  EXPECT_FALSE(DecodeClipboardOffer("[]"));
  EXPECT_FALSE(DecodeClipboardOffer(R"({"id":1,"request":1})"));  // neither a format nor skipped
  EXPECT_FALSE(DecodeClipboardOffer(R"({"id":1,"format":"text/plain","size":1})"));  // no request
  EXPECT_FALSE(DecodeClipboardOffer(R"({"id":-1,"request":1,"format":"text/plain","size":1})"));
}

TEST(ClipboardWire, DataAckCancel) {
  std::string_view bytes;
  const std::string d = EncodeClipboardData({0x1122334455667788, 0x100000000}, "payload");
  EXPECT_EQ(d.size(), kClipboardDataHeaderSize + 7);
  auto h = DecodeClipboardData(d, &bytes);
  ASSERT_TRUE(h);
  EXPECT_EQ(h->id, 0x1122334455667788u);
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
