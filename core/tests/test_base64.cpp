#include <gtest/gtest.h>

#include "rdmachine/base64.h"

namespace {

std::vector<uint8_t> Bytes(std::string_view s) { return {s.begin(), s.end()}; }

TEST(Base64, Rfc4648Vectors) {
  const std::pair<const char*, const char*> cases[] = {
      {"", ""},         {"f", "Zg=="},         {"fo", "Zm8="},        {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
  };
  for (auto [plain, encoded] : cases) {
    EXPECT_EQ(rdm::Base64Encode(Bytes(plain)), encoded);
    auto decoded = rdm::Base64Decode(encoded);
    ASSERT_TRUE(decoded) << encoded;
    EXPECT_EQ(*decoded, Bytes(plain));
  }
}

TEST(Base64, RoundTripsAllByteValues) {
  std::vector<uint8_t> all(256);
  for (int i = 0; i < 256; ++i) all[i] = uint8_t(i);
  auto decoded = rdm::Base64Decode(rdm::Base64Encode(all));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, all);
}

TEST(Base64, RejectsMalformed) {
  for (const char* bad : {"Zg=", "Zg", "Z===", "Zg==Zg==", "Zm9v!", "Zm=v", "Zh==", "Zm9=", " Zm9v"}) {
    EXPECT_FALSE(rdm::Base64Decode(bad)) << bad;
  }
}

}  // namespace
