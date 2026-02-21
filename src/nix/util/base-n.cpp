#include "nix/util/base-n.h"

#include <string_view>

#include "nix/util/array-from-string-literal.h"
#include "nix/util/util.h"

using namespace std::literals;
using namespace nix;

namespace nix {

constexpr static const std::array<char, 16> base16_chars = ARRAY_NO_NULL("0123456789abcdef");

std::string base16::encode(std::span<const std::byte> b) {
  std::string buf;
  buf.reserve(b.size() * 2);
  for (size_t i = 0; i < b.size(); i++) {
    buf.push_back(base16_chars[(uint8_t)b.data()[i] >> 4]);
    buf.push_back(base16_chars[(uint8_t)b.data()[i] & 0x0f]);
  }
  return buf;
}

std::string base16::decode(std::string_view s) {
  auto parse_hex_digit = [&](char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    throw FormatError("invalid character in Base16 string: '%c'", c);
  };

  if (s.size() % 2 != 0) {
    throw FormatError("Base16 string has odd length: %d", s.size());
  }
  auto decoded_size = s.size() / 2;

  std::string res;
  res.reserve(decoded_size);

  for (unsigned int i = 0; i < decoded_size; i++) {
    res.push_back(parse_hex_digit(s[i * 2]) << 4 | parse_hex_digit(s[i * 2 + 1]));
  }

  return res;
}

constexpr static const std::array<char, 64> base64_chars =
    ARRAY_NO_NULL("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");

std::string base64::encode(std::span<const std::byte> s) {
  std::string res;
  res.reserve((s.size() + 2) / 3 * 4);
  int data = 0, nbits = 0;

  for (std::byte c : s) {
    data = data << 8 | (uint8_t)c;
    nbits += 8;
    while (nbits >= 6) {
      nbits -= 6;
      res.push_back(base64_chars[data >> nbits & 0x3f]);
    }
  }

  if (nbits) {
    res.push_back(base64_chars[data << (6 - nbits) & 0x3f]);
  }
  while (res.size() % 4) {
    res.push_back('=');
  }

  return res;
}

std::string base64::decode(std::string_view s) {
  constexpr char npos = -1;
  constexpr std::array<char, 256> base64_decode_chars = [&] {
    std::array<char, 256> result{};
    for (auto& c : result) {
      c = npos;
    }
    for (int i = 0; i < 64; i++) {
      result[base64_chars[i]] = i;
    }
    return result;
  }();

  std::string res;
  // Some sequences are missing the padding consisting of up to two '='.
  //                    vvv
  res.reserve((s.size() + 2) / 4 * 3);
  unsigned int d = 0, bits = 0;

  for (char c : s) {
    if (c == '=') {
      break;
    }
    if (c == '\n') {
      continue;
    }

    char digit = base64_decode_chars[(unsigned char)c];
    if (digit == npos) {
      throw FormatError("invalid character in Base64 string: '%c'", c);
    }

    bits += 6;
    d = d << 6 | digit;
    if (bits >= 8) {
      res.push_back(d >> (bits - 8) & 0xff);
      bits -= 8;
    }
  }

  return res;
}

} // namespace nix
