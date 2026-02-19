// straylight::nix::primitives::checked_arithmetic
//
// Checked and saturating arithmetic primitives using compiler builtins.
// Replaces nix/util/checked-arithmetic.h with a more functional approach.
//
// Design philosophy:
// - checked_* functions return std::optional<T>, returning std::nullopt on overflow
// - saturating_* functions clamp to min/max instead of overflowing
// - Uses __builtin_*_overflow for optimal codegen on GCC/Clang
// - Fully constexpr where possible (C++23)
// - Header-only implementation

#pragma once

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// Implementation details
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// Detect compiler builtin support
#if defined(__GNUC__) || defined(__clang__)
#  define STRAYLIGHT_HAS_BUILTIN_OVERFLOW 1
#else
#  define STRAYLIGHT_HAS_BUILTIN_OVERFLOW 0
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Checked operations using builtins
// ─────────────────────────────────────────────────────────────────────────────

#if STRAYLIGHT_HAS_BUILTIN_OVERFLOW

template <std::integral T>
constexpr std::optional<T> checked_add_impl(T a, T b) noexcept {
  T result;
  if (__builtin_add_overflow(a, b, &result)) {
    return std::nullopt;
  }
  return result;
}

template <std::integral T>
constexpr std::optional<T> checked_sub_impl(T a, T b) noexcept {
  T result;
  if (__builtin_sub_overflow(a, b, &result)) {
    return std::nullopt;
  }
  return result;
}

template <std::integral T>
constexpr std::optional<T> checked_mul_impl(T a, T b) noexcept {
  T result;
  if (__builtin_mul_overflow(a, b, &result)) {
    return std::nullopt;
  }
  return result;
}

#else

// Fallback implementations without builtins

template <std::integral T>
constexpr std::optional<T> checked_add_impl(T a, T b) noexcept {
  if constexpr (std::is_unsigned_v<T>) {
    // Unsigned overflow: a + b overflows iff result < a
    T result = a + b;
    if (result < a) {
      return std::nullopt;
    }
    return result;
  } else {
    // Signed overflow check
    constexpr T max_val = std::numeric_limits<T>::max();
    constexpr T min_val = std::numeric_limits<T>::min();

    if (b > 0 && a > max_val - b) {
      return std::nullopt; // Positive overflow
    }
    if (b < 0 && a < min_val - b) {
      return std::nullopt; // Negative overflow
    }
    return a + b;
  }
}

template <std::integral T>
constexpr std::optional<T> checked_sub_impl(T a, T b) noexcept {
  if constexpr (std::is_unsigned_v<T>) {
    // Unsigned underflow: a - b underflows iff b > a
    if (b > a) {
      return std::nullopt;
    }
    return a - b;
  } else {
    // Signed overflow check
    constexpr T max_val = std::numeric_limits<T>::max();
    constexpr T min_val = std::numeric_limits<T>::min();

    if (b < 0 && a > max_val + b) {
      return std::nullopt; // Positive overflow
    }
    if (b > 0 && a < min_val + b) {
      return std::nullopt; // Negative overflow
    }
    return a - b;
  }
}

template <std::integral T>
constexpr std::optional<T> checked_mul_impl(T a, T b) noexcept {
  if constexpr (std::is_unsigned_v<T>) {
    if (a == 0 || b == 0) {
      return T{0};
    }
    // a * b overflows iff a > max / b
    if (a > std::numeric_limits<T>::max() / b) {
      return std::nullopt;
    }
    return a * b;
  } else {
    constexpr T max_val = std::numeric_limits<T>::max();
    constexpr T min_val = std::numeric_limits<T>::min();

    if (a == 0 || b == 0) {
      return T{0};
    }

    // Check all four sign combinations
    if (a > 0) {
      if (b > 0) {
        if (a > max_val / b) {
          return std::nullopt;
        }
      } else {
        if (b < min_val / a) {
          return std::nullopt;
        }
      }
    } else {
      if (b > 0) {
        if (a < min_val / b) {
          return std::nullopt;
        }
      } else {
        if (a != 0 && b < max_val / a) {
          return std::nullopt;
        }
      }
    }
    return a * b;
  }
}

#endif // STRAYLIGHT_HAS_BUILTIN_OVERFLOW

// ─────────────────────────────────────────────────────────────────────────────
// Saturating operations
// ─────────────────────────────────────────────────────────────────────────────

template <std::integral T>
constexpr T saturating_add_impl(T a, T b) noexcept {
  constexpr T max_val = std::numeric_limits<T>::max();
  constexpr T min_val = std::numeric_limits<T>::min();

#if STRAYLIGHT_HAS_BUILTIN_OVERFLOW
  T result;
  if (__builtin_add_overflow(a, b, &result)) {
    if constexpr (std::is_unsigned_v<T>) {
      return max_val;
    } else {
      // For signed: overflow direction depends on operand signs
      // If both positive or result would be positive, saturate to max
      // If both negative, saturate to min
      return (b > 0) ? max_val : min_val;
    }
  }
  return result;
#else
  if constexpr (std::is_unsigned_v<T>) {
    T result = a + b;
    if (result < a) {
      return max_val;
    }
    return result;
  } else {
    if (b > 0 && a > max_val - b) {
      return max_val;
    }
    if (b < 0 && a < min_val - b) {
      return min_val;
    }
    return a + b;
  }
#endif
}

template <std::integral T>
constexpr T saturating_sub_impl(T a, T b) noexcept {
  constexpr T max_val = std::numeric_limits<T>::max();
  constexpr T min_val = std::numeric_limits<T>::min();

#if STRAYLIGHT_HAS_BUILTIN_OVERFLOW
  T result;
  if (__builtin_sub_overflow(a, b, &result)) {
    if constexpr (std::is_unsigned_v<T>) {
      return T{0}; // Underflow saturates to 0 for unsigned
    } else {
      // For signed: underflow saturates to min, overflow to max
      return (b > 0) ? min_val : max_val;
    }
  }
  return result;
#else
  if constexpr (std::is_unsigned_v<T>) {
    if (b > a) {
      return T{0};
    }
    return a - b;
  } else {
    if (b > 0 && a < min_val + b) {
      return min_val;
    }
    if (b < 0 && a > max_val + b) {
      return max_val;
    }
    return a - b;
  }
#endif
}

template <std::integral T>
constexpr T saturating_mul_impl(T a, T b) noexcept {
  constexpr T max_val = std::numeric_limits<T>::max();
  constexpr T min_val = std::numeric_limits<T>::min();

#if STRAYLIGHT_HAS_BUILTIN_OVERFLOW
  T result;
  if (__builtin_mul_overflow(a, b, &result)) {
    if constexpr (std::is_unsigned_v<T>) {
      return max_val;
    } else {
      // For signed: result sign is XOR of operand signs
      bool result_negative = (a < 0) != (b < 0);
      return result_negative ? min_val : max_val;
    }
  }
  return result;
#else
  if (a == 0 || b == 0) {
    return T{0};
  }

  if constexpr (std::is_unsigned_v<T>) {
    if (a > max_val / b) {
      return max_val;
    }
    return a * b;
  } else {
    bool result_negative = (a < 0) != (b < 0);

    if (a > 0) {
      if (b > 0) {
        if (a > max_val / b) {
          return max_val;
        }
      } else {
        if (b < min_val / a) {
          return min_val;
        }
      }
    } else {
      if (b > 0) {
        if (a < min_val / b) {
          return min_val;
        }
      } else {
        if (a != 0 && b < max_val / a) {
          return max_val;
        }
      }
    }
    return a * b;
  }
#endif
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Public API: Checked arithmetic
// ─────────────────────────────────────────────────────────────────────────────

/// Checked addition. Returns std::nullopt on overflow.
template <std::integral T>
[[nodiscard]] constexpr std::optional<T> checked_add(T a, T b) noexcept {
  return detail::checked_add_impl(a, b);
}

/// Checked subtraction. Returns std::nullopt on overflow/underflow.
template <std::integral T>
[[nodiscard]] constexpr std::optional<T> checked_sub(T a, T b) noexcept {
  return detail::checked_sub_impl(a, b);
}

/// Checked multiplication. Returns std::nullopt on overflow.
template <std::integral T>
[[nodiscard]] constexpr std::optional<T> checked_mul(T a, T b) noexcept {
  return detail::checked_mul_impl(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API: Saturating arithmetic
// ─────────────────────────────────────────────────────────────────────────────

/// Saturating addition. Clamps to max on overflow, min on underflow (signed).
template <std::integral T>
[[nodiscard]] constexpr T saturating_add(T a, T b) noexcept {
  return detail::saturating_add_impl(a, b);
}

/// Saturating subtraction. Clamps to 0/min on underflow, max on overflow (signed).
template <std::integral T>
[[nodiscard]] constexpr T saturating_sub(T a, T b) noexcept {
  return detail::saturating_sub_impl(a, b);
}

/// Saturating multiplication. Clamps to max/min on overflow.
template <std::integral T>
[[nodiscard]] constexpr T saturating_mul(T a, T b) noexcept {
  return detail::saturating_mul_impl(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// Convenience type aliases
// ─────────────────────────────────────────────────────────────────────────────

// Type trait to get result type
template <std::integral T>
using checked_result = std::optional<T>;

} // namespace straylight::nix::primitives
