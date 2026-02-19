#pragma once
///@file

#include <algorithm>
#include <array>

namespace nix {

template <size_t sizeWithNull>
struct array_no_null_adaptor_t {
  std::array<char, sizeWithNull - 1> data;

  constexpr array_no_null_adaptor_t(const char (&init)[sizeWithNull]) {
    static_assert(sizeWithNull > 0);
    std::copy_n(init, sizeWithNull - 1, data.data());
  }
};

template <array_no_null_adaptor_t str>
constexpr auto operator""_arrayNoNull() {
  return str.data;
}

} // namespace nix
