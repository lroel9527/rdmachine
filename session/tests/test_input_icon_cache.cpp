#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "rdmachine/ime_wire.h"
#include "rdmachine/session/client_input_icon.h"

using rdm::InputIconCache;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> Png(uint8_t tag) {
  return {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', tag};
}

class InputIconCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tmpl = (fs::temp_directory_path() / "rdm-icons-XXXXXX").string();
    dir_ = mkdtemp(tmpl.data());
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  size_t Files() const {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir_)) n += e.path().extension() == ".png";
    return n;
  }
  fs::path dir_;
};

}  // namespace

TEST_F(InputIconCacheTest, KeepsAnIconByItsFingerprintAcrossProcesses) {
  const auto png = Png(1);
  const std::string fp = rdm::ImeIconFingerprint(png);
  {
    InputIconCache cache(dir_.string());
    EXPECT_FALSE(cache.Get(fp));
    EXPECT_TRUE(cache.Put(fp, png));
    EXPECT_EQ(cache.Get(fp), png);
  }
  InputIconCache later(dir_.string());  // another server run: from the disk
  EXPECT_EQ(later.Get(fp), png);
  EXPECT_EQ(Files(), 1u);
}

TEST_F(InputIconCacheTest, TakesOnlyAPngThatIsWhatItsFingerprintSays) {
  InputIconCache cache(dir_.string());
  const auto png = Png(2);
  EXPECT_FALSE(cache.Put(rdm::ImeIconFingerprint(Png(3)), png));       // another image's
  EXPECT_FALSE(cache.Put("../../" + rdm::ImeIconFingerprint(png), png));  // never a path
  const std::vector<uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a', 1, 2, 3};
  EXPECT_FALSE(cache.Put(rdm::ImeIconFingerprint(gif), gif));
  std::vector<uint8_t> big = Png(4);
  big.resize(rdm::kImeIconMaxBytes + 1);
  EXPECT_FALSE(cache.Put(rdm::ImeIconFingerprint(big), big));
  EXPECT_EQ(Files(), 0u);
  EXPECT_FALSE(cache.Get("../x"));
}

TEST_F(InputIconCacheTest, ADamagedFileIsAskedForAgain) {
  const auto png = Png(5);
  const std::string fp = rdm::ImeIconFingerprint(png);
  InputIconCache(dir_.string()).Put(fp, png);
  std::ofstream(dir_ / (fp + ".png"), std::ios::binary | std::ios::trunc) << "garbage";
  EXPECT_FALSE(InputIconCache(dir_.string()).Get(fp));
}

TEST_F(InputIconCacheTest, TheOldestGoBeyondTheBound) {
  InputIconCache cache(dir_.string(), /*max_files=*/3);
  std::vector<std::string> fps;
  for (uint8_t i = 0; i < 5; ++i) {
    const auto png = Png(10 + i);
    fps.push_back(rdm::ImeIconFingerprint(png));
    ASSERT_TRUE(cache.Put(fps.back(), png));
    cache.Flush();  // written on the cache's queue
    // Distinct times for the order (the file system's resolution).
    fs::last_write_time(dir_ / (fps.back() + ".png"), fs::file_time_type::clock::now() - std::chrono::hours(10 - i));
  }
  EXPECT_EQ(Files(), 3u);
  EXPECT_TRUE(fs::exists(dir_ / (fps[4] + ".png")));
  EXPECT_FALSE(fs::exists(dir_ / (fps[0] + ".png")));
  // In memory too, at most the bound.
  size_t here = 0;
  for (const auto& fp : fps) here += cache.Get(fp).has_value();
  EXPECT_EQ(here, 3u);
}

TEST_F(InputIconCacheTest, PutIsInMemoryAtOnce) {
  InputIconCache cache(dir_.string());
  const auto png = Png(30);
  ASSERT_TRUE(cache.Put(rdm::ImeIconFingerprint(png), png));
  EXPECT_EQ(cache.Get(rdm::ImeIconFingerprint(png)), png);  // before the disk has it
  cache.Flush();
  EXPECT_EQ(Files(), 1u);
}

TEST(InputIconCacheDir, IsUnderTheHome) {
  const std::string dir = rdm::InputIconCacheDir();
  EXPECT_NE(dir.find("/Library/Caches/org.rdmachine.server/input-icons"), std::string::npos);
}
