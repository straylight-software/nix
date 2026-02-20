// straylight::nix::crypto
//
// Implementation of high-performance base encoding primitives.
// Uses 256-byte lookup tables for O(1) decode performance.

#include "encoding.h"

#include <algorithm>
#include <cstdint>

namespace straylight::nix::crypto {

// ─────────────────────────────────────────────────────────────────────────────
// Base16 (Hexadecimal) implementation
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Decode lookup table: char -> value (0xFF = invalid)
constexpr std::array<uint8_t, 256> base16_decode_table = [] {
  std::array<uint8_t, 256> table{};
  for (auto& v : table) {
    v = 0xFF;
  }
  for (int i = 0; i < 10; ++i) {
    table['0' + i] = static_cast<uint8_t>(i);
  }
  for (int i = 0; i < 6; ++i) {
    table['a' + i] = static_cast<uint8_t>(10 + i);
    table['A' + i] = static_cast<uint8_t>(10 + i);
  }
  return table;
}();

} // namespace

std::string base16::encode(std::span<const uint8_t> data) {
  std::string result;
  result.reserve(data.size() * 2);

  for (uint8_t byte : data) {
    result += alphabet[(byte >> 4) & 0xF];
    result += alphabet[byte & 0xF];
  }

  return result;
}

std::vector<uint8_t> base16::decode(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    throw decode_error("Invalid hex string: odd length");
  }

  std::vector<uint8_t> result(hex.size() / 2);
  decode_to(hex, result);
  return result;
}

void base16::decode_to(std::string_view hex, std::span<uint8_t> out) {
  if (hex.size() != out.size() * 2) {
    throw decode_error("Invalid hex string length for output buffer");
  }

  for (std::size_t i = 0; i < out.size(); ++i) {
    uint8_t hi = base16_decode_table[static_cast<unsigned char>(hex[(i * 2)])];
    uint8_t lo = base16_decode_table[static_cast<unsigned char>(hex[(i * 2) + 1])];

    if (hi == 0xFF || lo == 0xFF) {
      throw decode_error("Invalid hex character");
    }

    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
}

bool base16::is_valid(std::string_view hex) noexcept {
  if (hex.size() % 2 != 0) {
    return false;
  }
  for (char c : hex) {
    if (base16_decode_table[static_cast<unsigned char>(c)] == 0xFF) {
      return false;
    }
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64 implementation (RFC 4648)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Decode lookup table: char -> value (0xFF = invalid, 0xFE = padding '=')
constexpr std::array<uint8_t, 256> base64_decode_table = [] {
  std::array<uint8_t, 256> table{};
  for (auto& v : table) {
    v = 0xFF;
  }
  for (int i = 0; i < 26; ++i) {
    table['A' + i] = static_cast<uint8_t>(i);
    table['a' + i] = static_cast<uint8_t>(26 + i);
  }
  for (int i = 0; i < 10; ++i) {
    table['0' + i] = static_cast<uint8_t>(52 + i);
  }
  table['+'] = 62;
  table['/'] = 63;
  table['='] = 0xFE; // padding marker
  return table;
}();

} // namespace

std::size_t base64::decoded_length(std::string_view encoded) noexcept {
  if (encoded.empty()) {
    return 0;
  }

  std::size_t padding = 0;
  if (encoded.size() >= 1 && encoded.back() == '=') {
    ++padding;
  }
  if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=') {
    ++padding;
  }

  return ((encoded.size() / 4) * 3) - padding;
}

std::string base64::encode(std::span<const uint8_t> data) {
  std::string result;
  result.reserve(encoded_length(data.size()));

  std::size_t i = 0;

  // Process 3-byte groups
  while (i + 2 < data.size()) {
    uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                      (static_cast<uint32_t>(data[i + 1]) << 8) |
                      static_cast<uint32_t>(data[i + 2]);
    result += alphabet[(triple >> 18) & 0x3F];
    result += alphabet[(triple >> 12) & 0x3F];
    result += alphabet[(triple >> 6) & 0x3F];
    result += alphabet[triple & 0x3F];
    i += 3;
  }

  // Handle remaining bytes with padding
  if (i + 1 == data.size()) {
    // 1 byte left -> 2 encoded chars + "=="
    uint32_t val = static_cast<uint32_t>(data[i]) << 16;
    result += alphabet[(val >> 18) & 0x3F];
    result += alphabet[(val >> 12) & 0x3F];
    result += "==";
  } else if (i + 2 == data.size()) {
    // 2 bytes left -> 3 encoded chars + "="
    uint32_t val =
        (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    result += alphabet[(val >> 18) & 0x3F];
    result += alphabet[(val >> 12) & 0x3F];
    result += alphabet[(val >> 6) & 0x3F];
    result += '=';
  }

  return result;
}

std::vector<uint8_t> base64::decode(std::string_view b64) {
  std::vector<uint8_t> result;
  result.reserve(decoded_length(b64));

  uint32_t buffer = 0;
  int bits = 0;

  for (char c : b64) {
    if (c == '=' || c == '\n') {
      continue;
    }

    uint8_t val = base64_decode_table[static_cast<unsigned char>(c)];
    if (val == 0xFF) {
      throw decode_error("Invalid base64 character");
    }

    buffer = (buffer << 6) | val;
    bits += 6;

    if (bits >= 8) {
      bits -= 8;
      result.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
    }
  }

  return result;
}

void base64::decode_to(std::string_view b64, std::span<uint8_t> out) {
  auto decoded = decode(b64);
  if (decoded.size() != out.size()) {
    throw decode_error("Base64 decoded size doesn't match output buffer");
  }
  std::copy(decoded.begin(), decoded.end(), out.begin());
}

bool base64::is_valid(std::string_view b64) noexcept {
  for (char c : b64) {
    uint8_t val = base64_decode_table[static_cast<unsigned char>(c)];
    if (val == 0xFF && c != '\n') {
      return false;
    }
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Nix32 implementation
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Decode lookup table: char -> value (0xFF = invalid)
constexpr std::array<uint8_t, 256> nix32_decode_table = [] {
  std::array<uint8_t, 256> table{};
  for (auto& v : table) {
    v = 0xFF;
  }
  // Map alphabet to values
  // Alphabet: 0123456789abcdfghijklmnpqrsvwxyz
  for (int i = 0; i < 10; ++i) {
    table['0' + i] = static_cast<uint8_t>(i);
  }
  // a=10, b=11, c=12, d=13, f=14 (skip e)
  table['a'] = 10;
  table['b'] = 11;
  table['c'] = 12;
  table['d'] = 13;
  table['f'] = 14;
  // g-n = 15-22 (skip o)
  for (int i = 0; i < 8; ++i) {
    table['g' + i] = static_cast<uint8_t>(15 + i);
  }
  // p-s = 23-26 (skip t, u)
  table['p'] = 23;
  table['q'] = 24;
  table['r'] = 25;
  table['s'] = 26;
  // v-z = 27-31
  table['v'] = 27;
  table['w'] = 28;
  table['x'] = 29;
  table['y'] = 30;
  table['z'] = 31;
  return table;
}();

} // namespace

uint8_t nix32::lookup_reverse(char c) noexcept {
  return nix32_decode_table[static_cast<unsigned char>(c)];
}

std::string nix32::encode(std::span<const uint8_t> data) {
  if (data.empty()) {
    return {};
  }

  // Output length: ceil(input_bits / 5)
  std::size_t len = encoded_length(data.size());
  std::string result(len, '0');

  // Nix32 encodes LSB first (reversed output)
  for (std::size_t i = 0; i < len; ++i) {
    std::size_t bit_pos = i * 5;
    std::size_t byte_pos = bit_pos / 8;
    std::size_t bit_offset = bit_pos % 8;

    uint8_t val = 0;
    if (byte_pos < data.size()) {
      val = data[byte_pos] >> bit_offset;
    }
    if (bit_offset > 3 && byte_pos + 1 < data.size()) {
      val |= data[byte_pos + 1] << (8 - bit_offset);
    }
    result[len - 1 - i] = alphabet[val & 0x1F];
  }

  return result;
}

std::vector<uint8_t> nix32::decode(std::string_view nix32_str) {
  if (nix32_str.empty()) {
    return {};
  }

  std::vector<uint8_t> result(decoded_length(nix32_str.size()), 0);
  decode_to(nix32_str, result);
  return result;
}

void nix32::decode_to(std::string_view nix32_str, std::span<uint8_t> out) {
  std::fill(out.begin(), out.end(), 0);

  for (std::size_t i = 0; i < nix32_str.size(); ++i) {
    uint8_t val = lookup_reverse(nix32_str[nix32_str.size() - 1 - i]);
    if (val == 0xFF) {
      throw decode_error("Invalid nix32 character");
    }

    std::size_t bit_pos = i * 5;
    std::size_t byte_pos = bit_pos / 8;
    std::size_t bit_offset = bit_pos % 8;

    if (byte_pos < out.size()) {
      out[byte_pos] |= val << bit_offset;
    }
    if (bit_offset > 3 && byte_pos + 1 < out.size()) {
      out[byte_pos + 1] |= val >> (8 - bit_offset);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SIMD-accelerated nix32 validation using AVX-512
//
// For NAR reference scanning, we need to validate 32-char nix32 strings quickly.
// Valid chars: 0-9, a-d, f, g-n, p-s, v-z (missing: e, o, t, u)
//
// Strategy: Use vectorized range checks instead of table lookup.
// A char is valid if:
//   - ('0' <= c <= '9') OR
//   - ('a' <= c <= 'd') OR
//   - (c == 'f') OR
//   - ('g' <= c <= 'n') OR
//   - ('p' <= c <= 's') OR
//   - ('v' <= c <= 'z')
// ─────────────────────────────────────────────────────────────────────────────

#if defined(__AVX512F__) && defined(__AVX512BW__)
#  include <immintrin.h>

// AVX-512 version: validate 64 characters at once
bool nix32_is_valid_avx512(const char* data, std::size_t len) noexcept {
  std::size_t i = 0;

  // Process 64 bytes at a time
  for (; i + 64 <= len; i += 64) {
    __m512i chars = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(data + i));

    // Check each valid range and OR them together
    // Range: '0'-'9' (0x30-0x39)
    __mmask64 is_digit = _mm512_cmpge_epu8_mask(chars, _mm512_set1_epi8('0')) &
                         _mm512_cmple_epu8_mask(chars, _mm512_set1_epi8('9'));

    // Range: 'a'-'d' (0x61-0x64)
    __mmask64 is_ad = _mm512_cmpge_epu8_mask(chars, _mm512_set1_epi8('a')) &
                      _mm512_cmple_epu8_mask(chars, _mm512_set1_epi8('d'));

    // Exact: 'f' (0x66)
    __mmask64 is_f = _mm512_cmpeq_epu8_mask(chars, _mm512_set1_epi8('f'));

    // Range: 'g'-'n' (0x67-0x6e)
    __mmask64 is_gn = _mm512_cmpge_epu8_mask(chars, _mm512_set1_epi8('g')) &
                      _mm512_cmple_epu8_mask(chars, _mm512_set1_epi8('n'));

    // Range: 'p'-'s' (0x70-0x73)
    __mmask64 is_ps = _mm512_cmpge_epu8_mask(chars, _mm512_set1_epi8('p')) &
                      _mm512_cmple_epu8_mask(chars, _mm512_set1_epi8('s'));

    // Range: 'v'-'z' (0x76-0x7a)
    __mmask64 is_vz = _mm512_cmpge_epu8_mask(chars, _mm512_set1_epi8('v')) &
                      _mm512_cmple_epu8_mask(chars, _mm512_set1_epi8('z'));

    // All chars must match at least one range
    __mmask64 valid = is_digit | is_ad | is_f | is_gn | is_ps | is_vz;

    if (valid != 0xFFFFFFFFFFFFFFFFULL) {
      return false;
    }
  }

  // Scalar fallback for remaining bytes
  for (; i < len; ++i) {
    if (nix32_decode_table[static_cast<unsigned char>(data[i])] == 0xFF) {
      return false;
    }
  }

  return true;
}

#elif defined(__AVX2__)
#  include <immintrin.h>

// AVX2 version: validate 32 characters at once
bool nix32_is_valid_avx2(const char* data, std::size_t len) noexcept {
  std::size_t i = 0;

  // Process 32 bytes at a time
  for (; i + 32 <= len; i += 32) {
    __m256i chars = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));

    // AVX2 doesn't have unsigned byte comparisons, so we use signed with bias
    // Bias by 128 to convert to signed range
    __m256i bias = _mm256_set1_epi8(-128);
    __m256i biased = _mm256_add_epi8(chars, bias);

    // Check each valid range (biased values)
    // Range: '0'-'9' -> -80 to -71
    __m256i lo_digit = _mm256_set1_epi8('0' - 128);
    __m256i hi_digit = _mm256_set1_epi8('9' - 128);
    __m256i is_digit =
        _mm256_and_si256(_mm256_cmpgt_epi8(biased, _mm256_sub_epi8(lo_digit, _mm256_set1_epi8(1))),
                         _mm256_cmpgt_epi8(_mm256_add_epi8(hi_digit, _mm256_set1_epi8(1)), biased));

    // Range: 'a'-'z' (we'll check for invalid chars within this range separately)
    __m256i lo_az = _mm256_set1_epi8('a' - 128);
    __m256i hi_z = _mm256_set1_epi8('z' - 128);
    __m256i is_az =
        _mm256_and_si256(_mm256_cmpgt_epi8(biased, _mm256_sub_epi8(lo_az, _mm256_set1_epi8(1))),
                         _mm256_cmpgt_epi8(_mm256_add_epi8(hi_z, _mm256_set1_epi8(1)), biased));

    // Check for invalid chars in a-z range: e, o, t, u
    __m256i is_e = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8('e'));
    __m256i is_o = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8('o'));
    __m256i is_t = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8('t'));
    __m256i is_u = _mm256_cmpeq_epi8(chars, _mm256_set1_epi8('u'));
    __m256i is_invalid = _mm256_or_si256(_mm256_or_si256(is_e, is_o), _mm256_or_si256(is_t, is_u));

    // Valid if (digit OR (a-z AND NOT invalid))
    __m256i valid = _mm256_or_si256(is_digit, _mm256_andnot_si256(is_invalid, is_az));

    // All bytes must be valid (all 1s)
    if (_mm256_movemask_epi8(valid) != static_cast<int>(0xFFFFFFFF)) {
      return false;
    }
  }

  // Scalar fallback for remaining bytes
  for (; i < len; ++i) {
    if (nix32_decode_table[static_cast<unsigned char>(data[i])] == 0xFF) {
      return false;
    }
  }

  return true;
}
#endif

bool nix32::is_valid(std::string_view nix32_str) noexcept {
#if defined(__AVX512F__) && defined(__AVX512BW__)
  return nix32_is_valid_avx512(nix32_str.data(), nix32_str.size());
#elif defined(__AVX2__)
  return nix32_is_valid_avx2(nix32_str.data(), nix32_str.size());
#else
  // Scalar fallback
  for (char c : nix32_str) {
    if (lookup_reverse(c) == 0xFF) {
      return false;
    }
  }
  return true;
#endif
}

} // namespace straylight::nix::crypto
