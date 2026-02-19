#ifndef NIX_UTIL_BASE_NIX_32_H
#define NIX_UTIL_BASE_NIX_32_H
///@file

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// NOLINTNEXTLINE(misc-include-cleaner)
#include "nix/util/array-from-string-literal.h"

namespace nix {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

/// bits per byte for base32 encoding calculation
constexpr size_t k_bits_per_byte = 8;
/// bits per base32 character
constexpr size_t k_bits_per_base32_char = 5;
struct base_nix32_t {
  /// omitted: E O U T
  constexpr static std::array<char, 32> characters =
      ARRAY_NO_NULL("0123456789abcdfghijklmnpqrsvwxyz");

private:
  // NOLINTNEXTLINE(readability-identifier-naming)
  static const std::array<uint8_t, 256> reverse_map_;

  const static constexpr uint8_t k_invalid = 0xFF;

public:
  [[nodiscard]] static auto lookup_reverse(char base32) -> std::optional<uint8_t> {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    uint8_t digit = reverse_map_[static_cast<unsigned char>(base32)];
    if (digit == k_invalid) {
      return std::nullopt;
    }
    return digit;
  }

  /**
   * Returns the length of a base-32 representation of this hash.
   */
  [[nodiscard]] constexpr static auto encoded_length(size_t original_length) -> size_t {
    return ((original_length * k_bits_per_byte - 1) / k_bits_per_base32_char) + 1;
  }

  [[nodiscard]] static auto encode(std::span<const std::byte> original_data) -> std::string;

  [[nodiscard]] static auto decode(std::string_view str) -> std::string;
};

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

} // namespace nix

#endif // NIX_UTIL_BASE_NIX_32_H
