#pragma once
///@file nix-language/runtime/wasm_executor.hh
/// Wasmtime-based WASM executor for compiled Nix expressions.
///
/// This module provides execution of compiled WASM modules using wasmtime.
/// It implements all the runtime imports required by the compiled code.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <wasmtime.hh>

#include "nix-language/compile/wasm_types.hh"
#include "nix-language/runtime/memory_layout.hh"
#include "nix-language/runtime/runtime.hh"

namespace nix::language::runtime {

// =============================================================================
// Execution Result
// =============================================================================

/// result of executing a WASM module
struct execution_result {
  bool success;
  nix_value value;      // result value (if success)
  std::string error;    // error message (if !success)
  std::uint32_t line;   // error line (if available)
  std::uint32_t column; // error column (if available)

  /// create a successful result
  static auto ok(nix_value v) -> execution_result { return {true, v, "", 0, 0}; }

  /// create an error result
  static auto err(std::string_view msg, std::uint32_t line = 0, std::uint32_t col = 0)
      -> execution_result {
    return {false, 0, std::string(msg), line, col};
  }
};

// =============================================================================
// WASM Executor
// =============================================================================

/// executes compiled WASM modules with runtime support
class wasm_executor {
public:
  wasm_executor();
  ~wasm_executor();

  // non-copyable, movable
  wasm_executor(const wasm_executor&) = delete;
  auto operator=(const wasm_executor&) -> wasm_executor& = delete;
  wasm_executor(wasm_executor&&) noexcept;
  auto operator=(wasm_executor&&) noexcept -> wasm_executor&;

  /// execute a WASM binary, returns the result of calling main()
  [[nodiscard]] auto execute(std::span<const std::uint8_t> wasm_binary) -> execution_result;

  /// execute a WASM binary from vector
  [[nodiscard]] auto execute(const std::vector<std::uint8_t>& wasm_binary) -> execution_result {
    return execute(std::span<const std::uint8_t>(wasm_binary));
  }

  /// get the runtime context (for inspection after execution)
  [[nodiscard]] auto context() const noexcept -> const runtime_context& { return ctx_; }

  /// read a string from the result memory
  [[nodiscard]] auto read_string_value(nix_value v) const -> std::string;

  /// format a value for display
  [[nodiscard]] auto format_value(nix_value v) const -> std::string;

public:
  // data stored in wasmtime::Store for callbacks to access
  struct store_data {
    runtime_context* ctx;
    wasmtime::Memory* memory;
  };

private:
  // wasmtime objects
  std::unique_ptr<wasmtime::Engine> engine_;
  std::unique_ptr<wasmtime::Store> store_;
  std::optional<wasmtime::Memory> memory_;
  std::optional<wasmtime::Instance> instance_;

  // our runtime context
  runtime_context ctx_;

  // store data for callbacks (includes pointer to memory for syncing)
  store_data store_data_;

  // setup methods
  void setup_linker(wasmtime::Linker& linker);
  void sync_memory_to_context();
  void sync_memory_from_context();
};

// =============================================================================
// Value Helpers
// =============================================================================

/// extract integer value (assumes is_int is true)
[[nodiscard]] inline auto get_int_value(nix_value v) -> std::int64_t {
  // payload is stored as 32-bit, sign-extend to 64-bit
  return static_cast<std::int64_t>(static_cast<std::int32_t>(get_payload(v)));
}

/// extract boolean value (assumes is_bool is true)
[[nodiscard]] inline auto get_bool_value(nix_value v) -> bool {
  return get_payload(v) != 0;
}

/// create an integer value
[[nodiscard]] inline auto make_int(std::int32_t n) -> nix_value {
  return make_value(value_tag::integer, static_cast<std::uint32_t>(n));
}

/// create a boolean value
[[nodiscard]] inline auto make_bool(bool b) -> nix_value {
  return b ? constants::bool_true : constants::bool_false;
}

} // namespace nix::language::runtime
