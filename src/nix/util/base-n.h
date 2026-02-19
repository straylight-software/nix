#pragma once
///@file

#include <span>
#include <string>

namespace nix {

namespace base16 {

/**
 * Returns the length of a base-16 representation of this many bytes.
 */
[[nodiscard]] constexpr static inline size_t encoded_length(size_t orig_size) {
  return orig_size * 2;
}

/**
 * Encode arbitrary bytes as base16.
 */
std::string encode(std::span<const std::byte> b);

/**
 * Decode arbitrary base16 string to bytes.
 */
std::string decode(std::string_view s);

} // namespace base16

namespace base64 {

/**
 * Returns the length of a base-64 representation of this many bytes.
 */
[[nodiscard]] constexpr static inline size_t encoded_length(size_t orig_size) {
  return ((4 * orig_size / 3) + 3) & ~3;
}

/**
 * Encode arbitrary bytes as base64.
 */
std::string encode(std::span<const std::byte> b);

/**
 * Decode arbitrary base64 string to bytes.
 */
std::string decode(std::string_view s);

} // namespace base64

} // namespace nix
