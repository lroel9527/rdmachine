#pragma once
// Checking a local account's password with the system (PAM on macOS and
// Linux), and the M0 rule that only the logged-in user may connect.
#include <string>

namespace rdm {

// The PAM service used when none is configured: "login" on Linux. On macOS
// "authorization" (Open Directory, the same check as unlocking a settings
// pane).
const char* DefaultPamService();

// Runs pam_authenticate and pam_acct_mgmt for `username` with `password`.
// Slow on failure by design (PAM's own delay). On failure *error is
// "bad_credentials" or "internal", and *detail says more for logs.
bool CheckSystemPassword(const std::string& service, const std::string& username, const std::string& password,
                         std::string* error, std::string* detail = nullptr);

// The account this process runs as (in M0, the user whose desktop is shared).
std::string CurrentUserName();

}  // namespace rdm
