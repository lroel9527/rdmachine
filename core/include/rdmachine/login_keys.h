#pragma once
// The keystrokes that log in at the login window or unlock the lock screen
// for a user who just logged in to rdmachine with a password (docs/
// decisions.md, "从登录窗口登录"). The server types them; the password is
// never stored.
//
// ASCII goes as physical keys (USB HID usages, US layout, which is what the
// login window uses unless the Mac was set up otherwise): a password field
// takes keys more reliably than typed characters. Anything else goes as a
// character.
#include <cstdint>
#include <string>
#include <vector>

namespace rdm {

struct LoginKey {
  enum class Kind { kKey, kText };
  Kind kind = Kind::kKey;
  uint32_t code = 0;   // kKey: HID usage (keyboard page); kText: Unicode scalar value
  bool shift = false;  // kKey: with Shift held
};

inline constexpr uint32_t kHidEnter = 0x28;
inline constexpr uint32_t kHidTab = 0x2B;
inline constexpr uint32_t kHidLeftShift = 0xE1;

// The password then Enter; with `type_name` (a login window that asks for
// the account name instead of showing a list of users): the name, Tab, the
// password, Enter. Malformed UTF-8 bytes are skipped.
std::vector<LoginKey> LoginKeystrokes(const std::string& username, const std::string& password, bool type_name);

// One character as a key on the US layout; false when it has none.
bool AsciiToHid(char c, uint32_t* usage, bool* shift);

}  // namespace rdm
