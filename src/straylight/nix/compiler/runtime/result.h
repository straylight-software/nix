#pragma once
/// @file straylight/nix/compiler/runtime/result.h
/// Sound Either monad for runtime error handling.
///
/// All runtime functions return `rt_result<T>` instead of throwing exceptions.
/// This ensures errors propagate explicitly through return values and are
/// impossible to ignore (via [[nodiscard]]).
///
/// Usage:
///   auto result = rt_force(ctx, value);
///   if (!result) return std::unexpected(result.error());
///   auto forced = *result;
///
/// Or with monadic operations:
///   return rt_force(ctx, value)
///       .and_then([&](nix_value v) { return rt_add(ctx, v, other); });

#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace straylight::nix::compiler::runtime {

// =============================================================================
// Error Types
// =============================================================================

/// Error kind for categorization
enum class rt_error_kind : std::uint8_t {
  generic,            // Unspecified error
  type_error,         // Type mismatch (e.g., adding string to int)
  attr_not_found,     // Attribute lookup failed
  assertion_failed,   // assert expression failed
  throw_error,        // builtins.throw was called
  abort_error,        // builtins.abort was called
  infinite_recursion, // Thunk evaluated itself
  out_of_memory,      // Heap exhausted
  io_error,           // File system operation failed
  import_error,       // Import failed
  division_by_zero,   // Division or modulo by zero
};

/// Runtime error value (not an exception)
///
/// This is a value type that can be returned, stored, and propagated.
/// It carries the error kind, message, and optional source position.
struct rt_error_t {
  rt_error_kind kind = rt_error_kind::generic;
  std::string message;
  std::uint32_t line = 0;
  std::uint32_t column = 0;

  // Constructors
  rt_error_t() = default;

  explicit rt_error_t(std::string msg, rt_error_kind k = rt_error_kind::generic)
      : kind(k), message(std::move(msg)) {}

  rt_error_t(std::string msg, std::uint32_t ln, std::uint32_t col,
             rt_error_kind k = rt_error_kind::generic)
      : kind(k), message(std::move(msg)), line(ln), column(col) {}

  // Named constructors for common errors
  [[nodiscard]] static auto type_error(std::string msg) -> rt_error_t {
    return rt_error_t(std::move(msg), rt_error_kind::type_error);
  }

  [[nodiscard]] static auto type_error(std::string msg, std::uint32_t ln, std::uint32_t col)
      -> rt_error_t {
    return rt_error_t(std::move(msg), ln, col, rt_error_kind::type_error);
  }

  [[nodiscard]] static auto attr_not_found(std::string msg) -> rt_error_t {
    return rt_error_t(std::move(msg), rt_error_kind::attr_not_found);
  }

  [[nodiscard]] static auto attr_not_found(std::string msg, std::uint32_t ln, std::uint32_t col)
      -> rt_error_t {
    return rt_error_t(std::move(msg), ln, col, rt_error_kind::attr_not_found);
  }

  [[nodiscard]] static auto assertion_failed(std::string msg, std::uint32_t ln, std::uint32_t col)
      -> rt_error_t {
    return rt_error_t(std::move(msg), ln, col, rt_error_kind::assertion_failed);
  }

  [[nodiscard]] static auto throw_error(std::string msg) -> rt_error_t {
    return rt_error_t(std::move(msg), rt_error_kind::throw_error);
  }

  [[nodiscard]] static auto abort_error(std::string msg) -> rt_error_t {
    return rt_error_t(std::move(msg), rt_error_kind::abort_error);
  }

  [[nodiscard]] static auto infinite_recursion(std::string msg) -> rt_error_t {
    return rt_error_t(std::move(msg), rt_error_kind::infinite_recursion);
  }

  [[nodiscard]] static auto out_of_memory(std::string msg) -> rt_error_t {
    return rt_error_t(std::move(msg), rt_error_kind::out_of_memory);
  }

  [[nodiscard]] static auto io_error(std::string msg) -> rt_error_t {
    return rt_error_t(std::move(msg), rt_error_kind::io_error);
  }

  [[nodiscard]] static auto import_error(std::string msg) -> rt_error_t {
    return rt_error_t(std::move(msg), rt_error_kind::import_error);
  }

  [[nodiscard]] static auto division_by_zero(std::uint32_t ln, std::uint32_t col) -> rt_error_t {
    return rt_error_t("division by zero", ln, col, rt_error_kind::division_by_zero);
  }

  // Predicates
  [[nodiscard]] auto is_catchable() const noexcept -> bool {
    // tryEval can catch throw errors but not aborts
    return kind == rt_error_kind::throw_error || kind == rt_error_kind::type_error ||
           kind == rt_error_kind::attr_not_found || kind == rt_error_kind::assertion_failed ||
           kind == rt_error_kind::io_error || kind == rt_error_kind::import_error ||
           kind == rt_error_kind::division_by_zero || kind == rt_error_kind::generic;
  }

  // Format for display
  [[nodiscard]] auto format() const -> std::string {
    if (line == 0 && column == 0) {
      return message;
    }
    return std::format("{} at line {}, column {}", message, line, column);
  }
};

// =============================================================================
// Result Type
// =============================================================================

/// Result type for runtime operations.
///
/// This is [[nodiscard]] to ensure callers handle errors.
/// Use .value() only when you're certain of success, otherwise
/// use operator* after checking or use monadic operations.
template <typename T>
struct [[nodiscard]] rt_result_t : std::expected<T, rt_error_t> {
  using base = std::expected<T, rt_error_t>;
  using base::base;

  // Allow implicit conversion from std::expected
  rt_result_t(base&& b) : base(std::move(b)) {}
  rt_result_t(const base& b) : base(b) {}

  // Convenience: create error result
  [[nodiscard]] static auto err(rt_error_t e) -> rt_result_t {
    return rt_result_t(std::unexpected(std::move(e)));
  }

  // Convenience: create success result
  [[nodiscard]] static auto ok(T val) -> rt_result_t { return rt_result_t(std::move(val)); }
};

/// Specialization for void results
template <>
struct [[nodiscard]] rt_result_t<void> : std::expected<void, rt_error_t> {
  using base = std::expected<void, rt_error_t>;
  using base::base;

  rt_result_t(base&& b) : base(std::move(b)) {}
  rt_result_t(const base& b) : base(b) {}

  [[nodiscard]] static auto err(rt_error_t e) -> rt_result_t {
    return rt_result_t(std::unexpected(std::move(e)));
  }

  [[nodiscard]] static auto ok() -> rt_result_t { return rt_result_t(); }
};

// =============================================================================
// Helper Macros
// =============================================================================

/// Propagate error if result is an error, otherwise unwrap the value.
/// Usage: auto value = RT_TRY(some_operation());
#define RT_TRY(expr)                                                                               \
  ({                                                                                               \
    auto&& _rt_result = (expr);                                                                    \
    if (!_rt_result)                                                                               \
      return std::unexpected(_rt_result.error());                                                  \
    std::move(*_rt_result);                                                                        \
  })

/// Propagate error if result is an error (for void results).
/// Usage: RT_TRY_VOID(some_void_operation());
#define RT_TRY_VOID(expr)                                                                          \
  do {                                                                                             \
    auto&& _rt_result = (expr);                                                                    \
    if (!_rt_result)                                                                               \
      return std::unexpected(_rt_result.error());                                                  \
  } while (0)

/// TEMPORARY: Unwrap result or throw (for incremental migration).
/// This should be removed once all functions return rt_result_t.
/// Usage: auto value = RT_UNWRAP(some_operation());
#define RT_UNWRAP(expr)                                                                            \
  ({                                                                                               \
    auto&& _rt_result = (expr);                                                                    \
    if (!_rt_result)                                                                               \
      throw std::runtime_error(_rt_result.error().format());                                       \
    std::move(*_rt_result);                                                                        \
  })

} // namespace straylight::nix::compiler::runtime
