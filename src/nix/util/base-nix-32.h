#pragma once
///@file

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "nix/util/array-from-string-literal.h"

namespace nix {

struct base_nix32_t {
  /// omitted: E O U T
  constexpr static std::array<char, 32> characters = "0123456789abcdfghijklmnpqrsvwxyz"_arrayNoNull;

private:
  static const std::array<uint8_t, 256> reverse_map;

  const static constexpr uint8_t invalid = 0xFF;

public:
  static inline std::optional<uint8_t> lookup_reverse(char base32) {
    uint8_t digit = reverse_map[static_cast<unsigned char>(base32)];
    if (digit == invalid)
      return std::nullopt;
    else
      return digit;
  }

  /**
   * Returns the length of a base-32 representation of this hash.
   */
  [[nodiscard]] constexpr static inline size_t encoded_length(size_t original_length) {
    return (original_length * 8 - 1) / 5 + 1;
  }

  static std::string encode(std::span<const std::byte> original_data);

  static std::string decode(std::string_view s);
};

} // namespace nix
