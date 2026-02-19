#ifndef NIX_UTIL_BASE_N_H
#define NIX_UTIL_BASE_N_H
///@file

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace nix {

namespace base16 {

/**
 * Returns the length of a base-16 representation of this many bytes.
 */
[[nodiscard]] constexpr static auto encoded_length(size_t orig_size) -> size_t {
  return orig_size * 2;
}

/**
 * Encode arbitrary bytes as base16.
 */
auto encode(std::span<const std::byte> bytes) -> std::string;

/**
 * Decode arbitrary base16 string to bytes.
 */
auto decode(std::string_view str) -> std::string;

} // namespace base16

namespace base64 {

/**
 * Returns the length of a base-64 representation of this many bytes.
 */
[[nodiscard]] constexpr static auto encoded_length(size_t orig_size) -> size_t {
  return ((4 * orig_size / 3) + 3) & ~3U;
}

/**
 * Encode arbitrary bytes as base64.
 */
auto encode(std::span<const std::byte> bytes) -> std::string;

/**
 * Decode arbitrary base64 string to bytes.
 */
auto decode(std::string_view str) -> std::string;

} // namespace base64

} // namespace nix

#endif // NIX_UTIL_BASE_N_H
