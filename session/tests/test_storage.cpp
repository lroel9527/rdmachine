#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>

#include "rdmachine/session/config_store.h"
#include "rdmachine/session/known_hosts.h"
#include "rdmachine/session/system_auth.h"

using namespace rdm;

namespace {

class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "rdm-test-XXXXXX").string();
    path_ = mkdtemp(tmpl.data());
  }
  ~TempDir() { std::filesystem::remove_all(path_); }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

int Mode(const std::string& p) {
  struct stat st {};
  stat(p.c_str(), &st);
  return st.st_mode & 0777;
}

}  // namespace

TEST(ConfigStore, CreatesIdentityOnceWithPrivateKey) {
  TempDir t;
  const std::string dir = t.path() + "/a/b";
  std::string error;
  auto first = LoadOrCreateServerIdentity(dir, "mac", &error);
  ASSERT_TRUE(first) << error;
  EXPECT_EQ(Mode(dir), 0700);
  EXPECT_EQ(Mode(dir + "/server-key.pem"), 0600);
  auto again = LoadOrCreateServerIdentity(dir, "mac", &error);
  ASSERT_TRUE(again) << error;
  EXPECT_EQ(again->cert_pem, first->cert_pem);
  EXPECT_EQ(again->key_pem, first->key_pem);
}

TEST(ConfigStore, RefusesHalfAnIdentity) {
  TempDir t;
  std::string error;
  ASSERT_TRUE(LoadOrCreateServerIdentity(t.path(), "mac", &error));
  std::filesystem::remove(t.path() + "/server-cert.pem");
  EXPECT_FALSE(LoadOrCreateServerIdentity(t.path(), "mac", &error));
  EXPECT_NE(error.find("only one"), std::string::npos);
}

TEST(ConfigStore, DefaultDirHonoursEnvironment) {
  setenv("RDMACHINE_CONFIG_DIR", "/tmp/somewhere", 1);
  EXPECT_EQ(DefaultConfigDir(), "/tmp/somewhere");
  unsetenv("RDMACHINE_CONFIG_DIR");
  EXPECT_NE(DefaultConfigDir().find("rdmachine"), std::string::npos);
}

TEST(KnownHosts, RememberCheckForget) {
  TempDir t;
  KnownHosts k(t.path() + "/sub/known_hosts");
  const std::string a = KnownHosts::Key("mac.local", 4100);
  const std::string b = KnownHosts::Key("fe80::1", 4100);
  EXPECT_EQ(b, "[fe80::1]:4100");
  EXPECT_EQ(k.Check(a, "sha256:aa"), KnownHosts::Verdict::kUnknown);
  std::string error;
  ASSERT_TRUE(k.Remember(a, "sha256:aa", &error)) << error;
  ASSERT_TRUE(k.Remember(b, "sha256:bb", &error)) << error;
  EXPECT_EQ(Mode(k.path()), 0600);
  EXPECT_EQ(k.Check(a, "sha256:aa"), KnownHosts::Verdict::kTrusted);
  EXPECT_EQ(k.Check(a, "sha256:cc"), KnownHosts::Verdict::kChanged);
  ASSERT_TRUE(k.Remember(a, "sha256:cc", &error));
  EXPECT_EQ(k.Lookup(a), "sha256:cc");
  ASSERT_TRUE(k.Forget(a, &error));
  EXPECT_FALSE(k.Lookup(a));
  EXPECT_EQ(k.Lookup(b), "sha256:bb");
}

TEST(KnownHosts, KeepsComments) {
  TempDir t;
  const std::string path = t.path() + "/known_hosts";
  std::string error;
  ASSERT_TRUE(WritePrivateFile(path, "# my servers\n\nmac:4100 sha256:aa\n", &error));
  KnownHosts k(path);
  ASSERT_TRUE(k.Remember("pc:4100", "sha256:bb", &error));
  auto text = ReadFile(path);
  ASSERT_TRUE(text);
  EXPECT_NE(text->find("# my servers"), std::string::npos);
  EXPECT_EQ(k.Lookup("mac:4100"), "sha256:aa");
}

// Real PAM, no mocks. The right-password check needs an account's password,
// so it runs only when RDM_TEST_PAM_USER and RDM_TEST_PAM_PASSWORD are set
// (run it by hand on a Mac: this is how the "authorization" service was
// found to reject every password).
TEST(SystemAuth, RealPamWithDefaultService) {
  const char* user = std::getenv("RDM_TEST_PAM_USER");
  const char* password = std::getenv("RDM_TEST_PAM_PASSWORD");
  if (!user || !password) GTEST_SKIP() << "set RDM_TEST_PAM_USER and RDM_TEST_PAM_PASSWORD";
  std::string error, detail;
  EXPECT_TRUE(rdm::CheckSystemPassword(rdm::DefaultPamService(), user, password, &error, &detail))
      << error << ": " << detail;
  EXPECT_FALSE(rdm::CheckSystemPassword(rdm::DefaultPamService(), user, std::string(password) + "x", &error, &detail));
  EXPECT_EQ(error, "bad_credentials") << detail;
}
