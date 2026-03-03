#pragma once

/// @file eval_adapter.h
/// @brief Adapter providing straylight evaluation for nix CLI commands
///
/// This adapter does NOT implement nix's eval_state_t (which is deeply tied to
/// the tree-walking interpreter and Boehm GC). Instead, it provides:
///
///   1. A standalone eval interface usable from CLI commands
///   2. Result conversion to nix value_t for interop where needed
///   3. JSON/string output for simple eval use cases
///
/// The straylight evaluator compiles Nix to WASM and executes it - a completely
/// different model from nix's tree-walking interpreter. Full interop would
/// require translating between value representations at runtime, which is
/// complex and potentially slow. For now, we focus on use cases where the
/// straylight evaluator can be used end-to-end.

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "nix/util/ref.h"

namespace nix {
class store_t;
class eval_state_t;
class value_t;
} // namespace nix

namespace straylight::nix::compiler {
class evaluator;
}

namespace straylight::nix::adapters {

// ============================================================================
// Error types
// ============================================================================

enum class eval_adapter_error {
  parse_error,
  compile_error,
  runtime_error,
  io_error,
  conversion_error,
};

struct eval_error_info {
  eval_adapter_error kind;
  std::string message;
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

template <typename T>
using eval_result = std::expected<T, eval_error_info>;

// ============================================================================
// Eval adapter
// ============================================================================

/// Adapter wrapping straylight's WASM-based evaluator for use from nix CLI.
///
/// This provides a simpler interface than eval_state_t, focused on:
///   - Evaluating expressions to JSON/string output
///   - File evaluation with import support
///   - Basic interop with nix store for path operations
class eval_adapter {
public:
  /// Construct with optional store reference for path operations.
  explicit eval_adapter(::nix::ref<::nix::store_t> store);

  /// Construct standalone (no store, pure evaluation only).
  eval_adapter();

  ~eval_adapter();

  // Non-copyable, movable
  eval_adapter(const eval_adapter&) = delete;
  eval_adapter& operator=(const eval_adapter&) = delete;
  eval_adapter(eval_adapter&&) noexcept;
  eval_adapter& operator=(eval_adapter&&) noexcept;

  // -------------------------------------------------------------------------
  // String evaluation (returns formatted result)
  // -------------------------------------------------------------------------

  /// Evaluate an expression string, return result as string.
  [[nodiscard]] auto eval_string(std::string_view expr) -> eval_result<std::string>;

  /// Evaluate a file, return result as string.
  [[nodiscard]] auto eval_file(const std::filesystem::path& path) -> eval_result<std::string>;

  // -------------------------------------------------------------------------
  // JSON evaluation
  // -------------------------------------------------------------------------

  /// Evaluate an expression string, return result as JSON.
  [[nodiscard]] auto eval_to_json(std::string_view expr) -> eval_result<std::string>;

  /// Evaluate a file, return result as JSON.
  [[nodiscard]] auto eval_file_to_json(const std::filesystem::path& path)
      -> eval_result<std::string>;

  // -------------------------------------------------------------------------
  // Raw evaluation (returns internal value representation)
  // -------------------------------------------------------------------------

  /// Evaluate an expression string, return raw value (int64_t tagged pointer).
  [[nodiscard]] auto eval_raw(std::string_view expr) -> eval_result<std::int64_t>;

  /// Evaluate a file, return raw value.
  [[nodiscard]] auto eval_file_raw(const std::filesystem::path& path) -> eval_result<std::int64_t>;

  // -------------------------------------------------------------------------
  // Nix value interop (limited)
  // -------------------------------------------------------------------------

  /// Evaluate and convert result to a nix value_t.
  /// Only works for simple types (int, float, bool, string, null).
  /// Complex types (attrsets, lists, functions) are not supported.
  ///
  /// @param expr Expression to evaluate
  /// @param state Nix eval state for value allocation
  /// @param result Output value (must be pre-allocated)
  /// @return true on success, false if conversion not possible
  [[nodiscard]] auto eval_to_nix_value(std::string_view expr, ::nix::eval_state_t& state,
                                       ::nix::value_t& result) -> eval_result<bool>;

  // -------------------------------------------------------------------------
  // Query
  // -------------------------------------------------------------------------

  /// Check if store is available for path operations.
  [[nodiscard]] bool has_store() const noexcept;

private:
  std::unique_ptr<compiler::evaluator> impl_;
  std::shared_ptr<::nix::store_t> store_;
};

// ============================================================================
// Factory
// ============================================================================

/// Create an eval adapter with store access.
[[nodiscard]] auto make_eval_adapter(::nix::ref<::nix::store_t> store)
    -> std::unique_ptr<eval_adapter>;

/// Create a standalone eval adapter (pure evaluation only).
[[nodiscard]] auto make_eval_adapter() -> std::unique_ptr<eval_adapter>;

/// Try to use straylight evaluator, fall back to nix evaluator.
/// Returns nullptr if straylight is not available or enabled.
[[nodiscard]] auto try_straylight_eval(std::string_view expr) -> eval_result<std::string>;

} // namespace straylight::nix::adapters
