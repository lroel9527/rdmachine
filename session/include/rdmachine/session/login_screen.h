#pragma once
// What stands between a remote user and the desktop on this computer: the
// lock screen of the session the server runs in, or the login window when
// the server runs in the login window session (docs/decisions.md, "登录窗口和
// 锁屏").
namespace rdm {

struct LoginScreenState {
  bool screen_locked = false;  // this session's lock screen is up
  // The login window asks for the account name (a name and a password
  // field) rather than showing a list of users.
  bool asks_for_name = false;
};

// macOS: the session's lock state and the login window's settings. Other
// platforms: nothing locked.
LoginScreenState QueryLoginScreen();

}  // namespace rdm
