#include "rdmachine/session/system_auth.h"

#include <pwd.h>
#include <security/pam_appl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace rdm {

const char* DefaultPamService() {
#if defined(__APPLE__)
  return "authorization";
#else
  return "login";
#endif
}

namespace {

struct Credentials {
  const std::string* username;
  const std::string* password;
};

// Answers PAM's prompts: the password for hidden input, the user name for
// visible input; messages are ignored.
int Converse(int num_msg, const struct pam_message** msg, struct pam_response** resp, void* appdata) {
  if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG) return PAM_CONV_ERR;
  auto* creds = static_cast<Credentials*>(appdata);
  auto* replies = static_cast<pam_response*>(std::calloc(static_cast<size_t>(num_msg), sizeof(pam_response)));
  if (!replies) return PAM_BUF_ERR;
  for (int i = 0; i < num_msg; ++i) {
    const char* answer = nullptr;
    switch (msg[i]->msg_style) {
      case PAM_PROMPT_ECHO_OFF:
        answer = creds->password->c_str();
        break;
      case PAM_PROMPT_ECHO_ON:
        answer = creds->username->c_str();
        break;
      default:
        break;
    }
    if (answer) {
      replies[i].resp = strdup(answer);
      if (!replies[i].resp) {
        for (int j = 0; j < i; ++j) std::free(replies[j].resp);
        std::free(replies);
        return PAM_BUF_ERR;
      }
    }
  }
  *resp = replies;
  return PAM_SUCCESS;
}

}  // namespace

bool CheckSystemPassword(const std::string& service, const std::string& username, const std::string& password,
                         std::string* error, std::string* detail) {
  Credentials creds{&username, &password};
  pam_conv conv{&Converse, &creds};
  pam_handle_t* pamh = nullptr;
  int ret = pam_start(service.c_str(), username.c_str(), &conv, &pamh);
  if (ret != PAM_SUCCESS) {
    if (error) *error = "internal";
    if (detail) *detail = "pam_start failed";
    return false;
  }
  ret = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK);
  if (ret == PAM_SUCCESS) ret = pam_acct_mgmt(pamh, PAM_DISALLOW_NULL_AUTHTOK);
  const bool ok = ret == PAM_SUCCESS;
  if (!ok) {
    if (detail) *detail = pam_strerror(pamh, ret);
    if (error) {
      *error = (ret == PAM_AUTH_ERR || ret == PAM_USER_UNKNOWN || ret == PAM_ACCT_EXPIRED ||
                ret == PAM_NEW_AUTHTOK_REQD || ret == PAM_PERM_DENIED || ret == PAM_MAXTRIES)
                   ? "bad_credentials"
                   : "internal";
    }
  }
  pam_end(pamh, ret);
  return ok;
}

std::string CurrentUserName() {
  std::vector<char> buf(16384);
  passwd pw{};
  passwd* result = nullptr;
  if (getpwuid_r(geteuid(), &pw, buf.data(), buf.size(), &result) == 0 && result) return result->pw_name;
  const char* u = std::getenv("USER");
  return u ? u : "";
}

}  // namespace rdm
