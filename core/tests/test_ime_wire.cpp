#include <gtest/gtest.h>

#include "rdmachine/ime_wire.h"

using rdm::ImeInput;
using rdm::ImeState;

TEST(ImeWire, InputRoundTrips) {
  const ImeInput cases[] = {
      {ImeInput::Kind::kMark, 1, "ni'hao", 6, 0, false},
      {ImeInput::Kind::kCommit, 2, "你好", 0, 0, false},
      {ImeInput::Kind::kCancel, 3, "", 0, 0, false},
      {ImeInput::Kind::kMode, 4, "", 0, 0, true},
      {ImeInput::Kind::kMode, 5, "", 0, 0, false},
  };
  for (const ImeInput& m : cases) {
    const auto back = rdm::DecodeImeInput(rdm::EncodeImeInput(m));
    ASSERT_TRUE(back) << rdm::EncodeImeInput(m);
    EXPECT_EQ(*back, m);
  }
  EXPECT_EQ(rdm::EncodeImeInput({ImeInput::Kind::kCommit, 7, "你好"}), R"({"commit":"你好","seq":7})");
}

TEST(ImeWire, InputMustBeExactlyOneThing) {
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"mark":"a"})"));                  // no seq
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1})"));                     // nothing
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"mark":"a","commit":"b"})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"mark":1})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"mark":"a","sel":[1]})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"cancel":false})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"mode":"maybe"})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":-1,"commit":"a"})"));
  EXPECT_FALSE(rdm::DecodeImeInput("not json"));
  const auto m = rdm::DecodeImeInput(R"({"seq":9,"mark":"ab"})");  // sel may be left out
  ASSERT_TRUE(m);
  EXPECT_EQ(m->sel_location, 0u);
}

TEST(ImeWire, KeysRoundTrip) {
  ImeInput mark{ImeInput::Kind::kMark, 5, "n", 1, 0};
  mark.ack = 3;
  ImeInput commit{ImeInput::Kind::kCommit, 6, "你"};
  commit.ack = 3;
  ImeInput key;
  key.kind = ImeInput::Kind::kKey;
  key.seq = 6;
  key.ack = 3;
  key.key = 11;
  key.im = true;
  key.field = true;
  key.events = {{5, 0x11, 0}};
  key.then = {mark, commit};
  ImeInput raw;
  raw.kind = ImeInput::Kind::kKey;
  raw.seq = 6;
  raw.key = 12;
  raw.field = false;
  raw.events = {{5, 0x39, 0}, {6, 0x39, 0x10}};
  ImeInput bare;  // a key-up that goes as a key: no field, nothing said
  bare.kind = ImeInput::Kind::kKey;
  bare.key = 13;
  for (const ImeInput& m : {key, raw, bare, mark}) {
    const auto back = rdm::DecodeImeInput(rdm::EncodeImeInput(m));
    ASSERT_TRUE(back) << rdm::EncodeImeInput(m);
    EXPECT_EQ(*back, m);
  }
  EXPECT_EQ(rdm::EncodeImeInput(raw), R"({"key":{"ev":[[5,57,0],[6,57,16]],"field":false,"id":12},"seq":6})");
}

TEST(ImeWire, KeysMustBeWellFormed) {
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"key":{}})"));                          // no id
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"key":{"id":1,"im":1}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"key":{"id":1,"ev":[[5,4]]}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"key":{"id":1,"ev":[[5,4,-1]]}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"key":{"id":1},"mark":"a"})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"ack":-2,"commit":"a"})"));
  // Only what an input method says goes in "then": no mode, no key.
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"key":{"id":1,"then":[{"seq":2,"mode":"on"}]}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"key":{"id":1,"then":[{"seq":2,"key":{"id":2}}]}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"key":{"id":1,"then":[{"mark":"a"}]}})"));
  EXPECT_TRUE(rdm::DecodeImeInput(R"({"seq":1,"key":{"id":1,"then":[{"seq":2,"cancel":true}]}})"));
}

TEST(ImeWire, StateRoundTrips) {
  ImeState s;
  s.available = true;
  s.client = true;
  s.app = "com.apple.TextEdit";
  s.seq = 12;
  s.field = 3;
  s.caret = rdm::ImeRect{1, 1312, 384, 2, 48};
  s.marked = rdm::ImeRange{0, 6};
  s.selected = rdm::ImeRange{6, 0};
  const auto back = rdm::DecodeImeState(rdm::EncodeImeState(s));
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, s);

  ImeState judged = s;  // 6.6.0
  judged.keys = true;
  judged.back = 41;
  judged.typed = 38;
  judged.judged = 44;
  EXPECT_EQ(*rdm::DecodeImeState(rdm::EncodeImeState(judged)), judged);

  ImeState badge = judged;  // 7.1.0
  badge.badge = true;
  badge.need_icon = "0123456789abcdef0123456789abcdef";
  EXPECT_EQ(*rdm::DecodeImeState(rdm::EncodeImeState(badge)), badge);
  // Only a fingerprint is wanted.
  EXPECT_TRUE(rdm::DecodeImeState(R"({"badge":true,"need_icon":"../x"})")->need_icon.empty());

  ImeState bare;
  EXPECT_EQ(rdm::EncodeImeState(bare), R"({"available":false,"client":false,"secure":false,"seq":0})");
  EXPECT_EQ(*rdm::DecodeImeState(rdm::EncodeImeState(bare)), bare);
}

// 7.1.0: the input source the client types with, for the server's menu bar.
TEST(ImeWire, SourceRoundTrips) {
  const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 1, 2, 255};
  ImeInput said;
  said.kind = ImeInput::Kind::kSource;
  said.seq = 9;
  said.ack = 3;
  said.source_id = "com.apple.inputmethod.SCIM.ITABC";
  said.icon = rdm::ImeIconFingerprint(png);
  EXPECT_EQ(*rdm::DecodeImeInput(rdm::EncodeImeInput(said)), said);
  ImeInput sent = said;  // with the image, once asked
  sent.png = png;
  EXPECT_EQ(*rdm::DecodeImeInput(rdm::EncodeImeInput(sent)), sent);
  ImeInput none = said;  // a source without an icon
  none.icon.clear();
  EXPECT_EQ(*rdm::DecodeImeInput(rdm::EncodeImeInput(none)), none);
}

TEST(ImeWire, SourceMustBeWellFormed) {
  const std::string fp = "0123456789abcdef0123456789abcdef";
  EXPECT_TRUE(rdm::DecodeImeInput(R"({"seq":1,"source":{"id":"x","icon":")" + fp + R"("}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"source":{"id":"x"}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"source":{"icon":")" + fp + R"("}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"source":{"id":"x","icon":"../../etc"}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"source":{"id":"x","icon":"0123456789ABCDEF0123456789ABCDEF"}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"source":{"id":"x","icon":")" + fp + R"(","png":"not base64!"}})"));
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"source":{"id":"x","icon":"","png":"iVBO"}})"));  // an image of no icon
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"source":{"id":"x","icon":")" + fp + R"("},"mode":"on"})"));
  EXPECT_FALSE(rdm::DecodeImeInput(  // not inside a key's "then"
      R"({"seq":1,"key":{"id":1,"then":[{"seq":1,"source":{"id":"x","icon":""}}]}})"));
  const std::string big((rdm::kImeIconMaxBytes / 3 + 2) * 4, 'A');  // valid base64, more than the largest image taken
  EXPECT_FALSE(rdm::DecodeImeInput(R"({"seq":1,"source":{"id":"x","icon":")" + fp + R"(","png":")" + big + R"("}})"));
}

TEST(ImeWire, IconFingerprintIsTheStartOfItsSha256) {
  // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
  EXPECT_EQ(rdm::ImeIconFingerprint({'a', 'b', 'c'}), "ba7816bf8f01cfea414140de5dae2223");
  EXPECT_TRUE(rdm::IsImeIconFingerprint(rdm::ImeIconFingerprint({})));
  EXPECT_FALSE(rdm::IsImeIconFingerprint("ba7816bf8f01cfea414140de5dae222"));
  EXPECT_FALSE(rdm::IsImeIconFingerprint("ba7816bf8f01cfea414140de5dae2223/"));
}

TEST(ImeWire, StateToleratesWhatItDoesNotKnow) {
  const auto s = rdm::DecodeImeState(R"({"available":"yes","caret":{"x":1},"marked":[1,2,3],"later":true})");
  ASSERT_TRUE(s);
  EXPECT_FALSE(s->available);
  EXPECT_FALSE(s->caret);
  EXPECT_FALSE(s->marked);
  EXPECT_FALSE(rdm::DecodeImeState("[1]"));
}

TEST(ImeWire, SocketPathIsUnderTheHome) {
  const std::string p = rdm::ImeSocketPath();
  ASSERT_FALSE(p.empty());
  EXPECT_NE(p.find("/Library/Application Support/RDMachine/ime.sock"), std::string::npos);
  EXPECT_LT(p.size(), 104u);  // sockaddr_un.sun_path
}
