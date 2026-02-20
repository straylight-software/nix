// straylight // nix-language
//
// Full Nix Evaluator - combines parser, compiler, and WASM executor
//
// This is the top-level interface for evaluating Nix expressions.
// It provides:
//   - Single expression evaluation
//   - File evaluation with import support
//   - Import caching and cycle detection
//
// Usage:
//   evaluator eval;
//   auto result = eval.eval_string("1 + 2");
//   auto result = eval.eval_file("./default.nix");

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "straylight/nix/compiler/runtime/io_config.h"

namespace straylight::nix::compiler {

// Forward declarations
namespace runtime {
class wasm_executor;
class io_backend_interface;
struct execution_result;
} // namespace runtime

namespace compile {
using nix_value = std::int64_t;
}

// ─────────────────────────────────────────────────────────────────────────────
// Evaluator Error Types
// ─────────────────────────────────────────────────────────────────────────────

enum class eval_error_kind : std::uint8_t {
  parse_error,
  compile_error,
  runtime_error,
  io_error,
};

struct eval_error {
  eval_error_kind kind;
  std::string message;
  std::string file;     // file where error occurred (if applicable)
  std::uint32_t line;   // 0 if unknown
  std::uint32_t column; // 0 if unknown
};

template <typename T>
using eval_result = std::expected<T, eval_error>;

// ─────────────────────────────────────────────────────────────────────────────
// Evaluator
// ─────────────────────────────────────────────────────────────────────────────

/// Full Nix evaluator combining parser, compiler, and WASM execution.
///
/// This is the main entry point for evaluating Nix expressions.
/// It handles import resolution, caching, and the full evaluation pipeline.
class evaluator {
public:
  /// Construct with default I/O backend (based on compile-time config).
  evaluator();

  /// Construct with explicit I/O backend.
  explicit evaluator(std::unique_ptr<runtime::io_backend_interface> io);

  ~evaluator();

  // Non-copyable, movable
  evaluator(const evaluator&) = delete;
  auto operator=(const evaluator&) -> evaluator& = delete;
  evaluator(evaluator&&) noexcept;
  auto operator=(evaluator&&) noexcept -> evaluator&;

  // -------------------------------------------------------------------------
  // Evaluation
  // -------------------------------------------------------------------------

  /// Evaluate a Nix expression string.
  /// Returns the result as a formatted string.
  [[nodiscard]] auto eval_string(std::string_view source) -> eval_result<std::string>;

  /// Evaluate a Nix expression string and return raw value.
  [[nodiscard]] auto eval_string_raw(std::string_view source) -> eval_result<compile::nix_value>;

  /// Evaluate a Nix file.
  /// The file path is used for import resolution.
  [[nodiscard]] auto eval_file(const std::filesystem::path& path) -> eval_result<std::string>;

  /// Evaluate a Nix file and return raw value.
  [[nodiscard]] auto eval_file_raw(const std::filesystem::path& path)
      -> eval_result<compile::nix_value>;

  // -------------------------------------------------------------------------
  // Internal (for import callback)
  // -------------------------------------------------------------------------

  /// Evaluate source code from a file path.
  /// Used internally by the import callback.
  [[nodiscard]] auto eval_source(std::string_view source, std::string_view path)
      -> compile::nix_value;

private:
  std::unique_ptr<runtime::wasm_executor> executor_;

  /// Compile Nix source to WASM binary.
  [[nodiscard]] auto compile_source(std::string_view source, std::string_view path)
      -> eval_result<std::vector<std::uint8_t>>;
};

} // namespace straylight::nix::compiler
