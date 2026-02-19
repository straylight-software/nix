// straylight::nix::primitives::encoding
//
// High-performance base encoding primitives for Nix store paths and hashes.
//
// Supported encodings:
//   - Base16 (hex): Standard lowercase hexadecimal
//   - Base64 (RFC 4648): Standard base64 with padding
//   - Nix32: Nix-specific base32 variant (omits e, o, u, t)
//
// All encoders use 256-byte lookup tables for O(1) decode performance.
// Nix32 encoding is LSB-first (reversed) to match nix store path format.
//
// Usage:
//   std::vector<uint8_t> data = {...};
//   auto hex = base16::encode(data);
//   auto decoded = base16::decode(hex);
//
//   auto b64 = base64::encode(data);
//   auto nix = nix32::encode(data);

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace straylight::nix::primitives::encoding {

// ─────────────────────────────────────────────────────────────────────────────
// Error types
// ─────────────────────────────────────────────────────────────────────────────

/// Exception thrown when decoding encounters invalid input
class decode_error : public std::runtime_error {
public:
  explicit decode_error(const char* what) : std::runtime_error(what) {}
  explicit decode_error(const std::string& what) : std::runtime_error(what) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Base16 (Hexadecimal) encoding
// ─────────────────────────────────────────────────────────────────────────────

namespace base16 {

/// Characters used for encoding (lowercase)
inline constexpr std::array<char, 16> alphabet = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

/// Calculate encoded length for given input size
[[nodiscard]] constexpr std::size_t encoded_length(std::size_t input_size) noexcept {
  return input_size * 2;
}

/// Calculate decoded length for given encoded size
[[nodiscard]] constexpr std::size_t decoded_length(std::size_t encoded_size) noexcept {
  return encoded_size / 2;
}

/// Encode binary data to lowercase hexadecimal string
[[nodiscard]] std::string encode(std::span<const uint8_t> data);

/// Encode binary data (from std::byte span) to lowercase hexadecimal string
[[nodiscard]] inline std::string encode(std::span<const std::byte> data) {
  return encode(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

/// Decode hexadecimal string to binary data
/// @throws decode_error if input contains invalid characters or has odd length
[[nodiscard]] std::vector<uint8_t> decode(std::string_view hex);

/// Decode hexadecimal string into pre-allocated buffer
/// @throws decode_error if input is invalid or output size doesn't match
void decode_to(std::string_view hex, std::span<uint8_t> out);

/// Check if a character is a valid hex digit
[[nodiscard]] constexpr bool is_valid_char(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// Check if a string is valid hexadecimal
[[nodiscard]] bool is_valid(std::string_view hex) noexcept;

} // namespace base16

// ─────────────────────────────────────────────────────────────────────────────
// Base64 encoding (RFC 4648)
// ─────────────────────────────────────────────────────────────────────────────

namespace base64 {

/// Characters used for encoding (standard alphabet)
inline constexpr std::array<char, 64> alphabet = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

/// Calculate encoded length for given input size (with padding)
[[nodiscard]] constexpr std::size_t encoded_length(std::size_t input_size) noexcept {
  return ((input_size + 2) / 3) * 4;
}

/// Calculate decoded length for given encoded string
/// Accounts for padding characters
[[nodiscard]] std::size_t decoded_length(std::string_view encoded) noexcept;

/// Encode binary data to base64 string (with padding)
[[nodiscard]] std::string encode(std::span<const uint8_t> data);

/// Encode binary data (from std::byte span) to base64 string
[[nodiscard]] inline std::string encode(std::span<const std::byte> data) {
  return encode(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

/// Decode base64 string to binary data
/// Handles padding and ignores newlines
/// @throws decode_error if input contains invalid characters
[[nodiscard]] std::vector<uint8_t> decode(std::string_view b64);

/// Decode base64 string into pre-allocated buffer
/// @throws decode_error if input is invalid or output size doesn't match
void decode_to(std::string_view b64, std::span<uint8_t> out);

/// Check if a character is a valid base64 character (including padding)
[[nodiscard]] constexpr bool is_valid_char(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
         c == '/' || c == '=' || c == '\n';
}

/// Check if a string is valid base64
[[nodiscard]] bool is_valid(std::string_view b64) noexcept;

} // namespace base64

// ─────────────────────────────────────────────────────────────────────────────
// Nix32 encoding (Nix-specific base32 variant)
//
// Nix uses a custom base32 alphabet that omits e, o, u, t to avoid
// potentially offensive words. The encoding is also LSB-first (reversed).
//
// Alphabet: 0123456789abcdfghijklmnpqrsvwxyz (32 characters)
// ─────────────────────────────────────────────────────────────────────────────

namespace nix32 {

/// Characters used for encoding (omits e, o, u, t)
inline constexpr std::array<char, 32> alphabet = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'f', 'g',
    'h', 'i', 'j', 'k', 'l', 'm', 'n', 'p', 'q', 'r', 's', 'v', 'w', 'x', 'y', 'z'};

/// Calculate encoded length for given input size
/// Formula: ceil(input_bits / 5) = ceil(input_size * 8 / 5)
[[nodiscard]] constexpr std::size_t encoded_length(std::size_t input_size) noexcept {
  if (input_size == 0) {
    return 0;
  }
  return ((input_size * 8) + 4) / 5;
}

/// Calculate decoded length for given encoded size
/// Formula: floor(encoded_bits / 8) = floor(encoded_size * 5 / 8)
[[nodiscard]] constexpr std::size_t decoded_length(std::size_t encoded_size) noexcept {
  return (encoded_size * 5) / 8;
}

/// Encode binary data to nix32 string
[[nodiscard]] std::string encode(std::span<const uint8_t> data);

/// Encode binary data (from std::byte span) to nix32 string
[[nodiscard]] inline std::string encode(std::span<const std::byte> data) {
  return encode(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

/// Decode nix32 string to binary data
/// @throws decode_error if input contains invalid characters
[[nodiscard]] std::vector<uint8_t> decode(std::string_view nix32_str);

/// Decode nix32 string into pre-allocated buffer
/// @throws decode_error if input is invalid or output size doesn't match
void decode_to(std::string_view nix32_str, std::span<uint8_t> out);

/// Lookup table for reverse mapping (char -> value)
/// Returns 0xFF for invalid characters
[[nodiscard]] uint8_t lookup_reverse(char c) noexcept;

/// Check if a character is a valid nix32 character
[[nodiscard]] constexpr bool is_valid_char(char c) noexcept {
  // Valid: 0-9, a-d, f, g-n, p-s, v-z
  // Invalid: e, o, t, u
  if (c >= '0' && c <= '9') {
    return true;
  }
  if (c >= 'a' && c <= 'z') {
    return c != 'e' && c != 'o' && c != 't' && c != 'u';
  }
  return false;
}

/// Check if a string is valid nix32
[[nodiscard]] bool is_valid(std::string_view nix32_str) noexcept;

} // namespace nix32

} // namespace straylight::nix::primitives::encoding
