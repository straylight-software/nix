#pragma once
///@file

#include <bit>
#include <cassert>
#include <cstdint>
#include <type_traits>

namespace nix {

/// Aligns val upwards to be a multiple of alignment.
///
/// @pre alignment must be a power of 2.
template <typename T>
  requires std::is_unsigned_v<T>
constexpr T align_up(T val, unsigned alignment) {
  assert(std::has_single_bit(alignment) && "alignment must be a power of 2");
  T mask = ~(T{alignment} - 1u);
  return (val + alignment - 1) & mask;
}

} // namespace nix
