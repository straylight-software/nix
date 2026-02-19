#ifndef NIX_UTIL_ALIGNMENT_H
#define NIX_UTIL_ALIGNMENT_H
///@file

#include <bit>
#include <cassert>
#include <type_traits>

namespace nix {

/// Aligns val upwards to be a multiple of alignment.
///
/// @pre alignment must be a power of 2.
template <typename T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr auto align_up(T val, unsigned alignment) -> T {
  assert(std::has_single_bit(alignment) && "alignment must be a power of 2");
  T mask = ~(T{alignment} - 1U);
  return (val + alignment - 1U) & mask;
}

} // namespace nix

#endif // NIX_UTIL_ALIGNMENT_H
