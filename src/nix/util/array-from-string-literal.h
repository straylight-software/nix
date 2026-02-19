#ifndef NIX_UTIL_ARRAY_FROM_STRING_LITERAL_H
#define NIX_UTIL_ARRAY_FROM_STRING_LITERAL_H
///@file

#include <algorithm>
#include <array>
#include <cstddef>

namespace nix {

template <std::size_t SizeWithNull>
struct array_no_null_adaptor_t {
  // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
  std::array<char, SizeWithNull - 1> data;

  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  constexpr explicit array_no_null_adaptor_t(const char (&init)[SizeWithNull]) {
    static_assert(SizeWithNull > 0);
    std::copy_n(init, SizeWithNull - 1, data.data());
  }
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
};

// CTAD guide for string literal deduction
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
template <std::size_t N>
array_no_null_adaptor_t(const char (&)[N]) -> array_no_null_adaptor_t<N>;
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)

// Note: The C++20 class NTTP literal operator syntax requires the
// string literal to be wrapped in a class type. The template deduction
// happens via the implicit conversion from string literal to the adaptor type.
// However, some compilers may not support this fully. As a workaround,
// we provide both the NTTP form and a helper macro.

template <array_no_null_adaptor_t Str>
[[nodiscard]] constexpr auto operator""_array_no_null() {
  return Str.data;
}

// CamelCase alias for backward compatibility
template <array_no_null_adaptor_t Str>
[[nodiscard]] constexpr auto operator""_arrayNoNull() {
  return Str.data;
}

// Helper macro for compilers that don't fully support NTTP literal operators
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define ARRAY_NO_NULL(str) (::nix::array_no_null_adaptor_t(str).data)
// NOLINTEND(cppcoreguidelines-macro-usage)

} // namespace nix

#endif // NIX_UTIL_ARRAY_FROM_STRING_LITERAL_H
