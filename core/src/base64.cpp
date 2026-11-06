#include "rdmachine/base64.h"

namespace rdm {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

std::string Base64Encode(std::span<const uint8_t> bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const uint32_t v = (uint32_t{bytes[i]} << 16) | (uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  const size_t rest = bytes.size() - i;
  if (rest == 1) {
    const uint32_t v = uint32_t{bytes[i]} << 16;
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += "==";
  } else if (rest == 2) {
    const uint32_t v = (uint32_t{bytes[i]} << 16) | (uint32_t{bytes[i + 1]} << 8);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

std::optional<std::vector<uint8_t>> Base64Decode(std::string_view text) {
  if (text.size() % 4 != 0) return std::nullopt;
  std::vector<uint8_t> out;
  out.reserve(text.size() / 4 * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    const bool last = i + 4 == text.size();
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = text[i + k];
      if (c == '=') {
        // Padding only in the last group, only in the last two places, and
        // nothing but padding after it.
        if (!last || k < 2) return std::nullopt;
        ++pad;
        v[k] = 0;
      } else {
        if (pad > 0) return std::nullopt;
        v[k] = Value(c);
        if (v[k] < 0) return std::nullopt;
      }
    }
    const uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
    out.push_back(uint8_t(n >> 16));
    if (pad < 2) out.push_back(uint8_t(n >> 8));
    if (pad < 1) out.push_back(uint8_t(n));
    // Reject non-canonical encodings (bits set in the padding positions).
    if (pad == 2 && (n & 0xFFFF) != 0) return std::nullopt;
    if (pad == 1 && (n & 0xFF) != 0) return std::nullopt;
  }
  return out;
}

}  // namespace rdm
