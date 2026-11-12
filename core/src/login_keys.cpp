#include "rdmachine/login_keys.h"

namespace rdm {

bool AsciiToHid(char c, uint32_t* usage, bool* shift) {
  *shift = false;
  if (c >= 'a' && c <= 'z') {
    *usage = 0x04 + static_cast<uint32_t>(c - 'a');
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    *usage = 0x04 + static_cast<uint32_t>(c - 'A');
    *shift = true;
    return true;
  }
  if (c >= '1' && c <= '9') {
    *usage = 0x1E + static_cast<uint32_t>(c - '1');
    return true;
  }
  struct Entry {
    char c;
    uint32_t usage;
    bool shift;
  };
  static constexpr Entry kTable[] = {
      {'0', 0x27, false}, {' ', 0x2C, false}, {'-', 0x2D, false},  {'=', 0x2E, false}, {'[', 0x2F, false},
      {']', 0x30, false}, {'\\', 0x31, false}, {';', 0x33, false}, {'\'', 0x34, false}, {'`', 0x35, false},
      {',', 0x36, false}, {'.', 0x37, false}, {'/', 0x38, false},  {'!', 0x1E, true},   {'@', 0x1F, true},
      {'#', 0x20, true},  {'$', 0x21, true},  {'%', 0x22, true},   {'^', 0x23, true},   {'&', 0x24, true},
      {'*', 0x25, true},  {'(', 0x26, true},  {')', 0x27, true},   {'_', 0x2D, true},   {'+', 0x2E, true},
      {'{', 0x2F, true},  {'}', 0x30, true},  {'|', 0x31, true},   {':', 0x33, true},   {'"', 0x34, true},
      {'~', 0x35, true},  {'<', 0x36, true},  {'>', 0x37, true},   {'?', 0x38, true},
  };
  for (const Entry& e : kTable) {
    if (e.c == c) {
      *usage = e.usage;
      *shift = e.shift;
      return true;
    }
  }
  return false;
}

namespace {

// Appends the keys for `s`: ASCII as keys, the rest as characters.
void Type(const std::string& s, std::vector<LoginKey>* out) {
  for (size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
      uint32_t usage = 0;
      bool shift = false;
      if (AsciiToHid(static_cast<char>(b), &usage, &shift)) {
        out->push_back({LoginKey::Kind::kKey, usage, shift});
      } else {
        out->push_back({LoginKey::Kind::kText, b, false});  // control characters and the like
      }
      ++i;
      continue;
    }
    int len = 0;
    uint32_t cp = 0;
    if ((b & 0xE0) == 0xC0) {
      len = 2;
      cp = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
      len = 3;
      cp = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
      len = 4;
      cp = b & 0x07;
    } else {
      ++i;  // a stray continuation byte
      continue;
    }
    bool ok = i + static_cast<size_t>(len) <= s.size();
    for (int k = 1; ok && k < len; ++k) {
      const auto c = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
      if ((c & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (c & 0x3F);
    }
    if (ok && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) out->push_back({LoginKey::Kind::kText, cp, false});
    i += ok ? static_cast<size_t>(len) : 1;
  }
}

}  // namespace

std::vector<LoginKey> LoginKeystrokes(const std::string& username, const std::string& password, bool type_name) {
  std::vector<LoginKey> keys;
  if (type_name) {
    Type(username, &keys);
    keys.push_back({LoginKey::Kind::kKey, kHidTab, false});
  }
  Type(password, &keys);
  keys.push_back({LoginKey::Kind::kKey, kHidEnter, false});
  return keys;
}

}  // namespace rdm
