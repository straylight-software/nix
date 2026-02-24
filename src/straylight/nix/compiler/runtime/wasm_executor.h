#pragma once
///@file straylight/nix/compiler/runtime/wasm_executor.h
/// Wasmtime-based WASM executor for compiled Nix expressions.
///
/// This module provides execution of compiled WASM modules using wasmtime.
/// It implements all the runtime imports required by the compiled code.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <wasmtime.hh>

#include "straylight/nix/compiler/compile/wasm_types.h"
#include "straylight/nix/compiler/runtime/io_config.h"
#include "straylight/nix/compiler/runtime/memory_layout.h"
#include "straylight/nix/compiler/runtime/runtime.h"
#include "straylight/nix/compiler/runtime/wasm_memory.h"

namespace straylight::nix::compiler::runtime {

// Forward declaration
class io_backend_interface;

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
  /// Construct with default I/O backend (based on compile-time config)
  wasm_executor();

  /// Construct with explicit I/O backend (takes ownership)
  explicit wasm_executor(std::unique_ptr<io_backend_interface> io);

  ~wasm_executor();

  // non-copyable, movable
  wasm_executor(const wasm_executor&) = delete;
  auto operator=(const wasm_executor&) -> wasm_executor& = delete;
  wasm_executor(wasm_executor&&) noexcept;
  auto operator=(wasm_executor&&) noexcept -> wasm_executor&;

  /// execute a WASM binary, returns the result of calling main()
  /// This creates a fresh store/memory/context for each call.
  [[nodiscard]] auto execute(std::span<const std::uint8_t> wasm_binary) -> execution_result;

  /// execute a WASM binary from vector
  [[nodiscard]] auto execute(const std::vector<std::uint8_t>& wasm_binary) -> execution_result {
    return execute(std::span<const std::uint8_t>(wasm_binary));
  }

  /// Execute a WASM binary within the existing context.
  /// This reuses the current store/memory, suitable for imports.
  /// Must be called during an existing execute() call.
  [[nodiscard]] auto execute_within(std::span<const std::uint8_t> wasm_binary) -> execution_result;

  /// Check if there is an active execution context.
  [[nodiscard]] auto has_context() const noexcept -> bool { return store_ != nullptr; }

  /// get the runtime context (for inspection after execution)
  [[nodiscard]] auto context() const noexcept -> const runtime_context& { return ctx_; }

  /// Get the I/O backend (for configuration, e.g., setting import callback)
  [[nodiscard]] auto io() const noexcept -> io_backend_interface* { return io_.get(); }

  /// read a string from the result memory
  [[nodiscard]] auto read_string_value(nix_value v) const -> std::string;

  /// format a value for display
  [[nodiscard]] auto format_value(nix_value v) const -> std::string;

public:
  // data stored in wasmtime::Store for callbacks to access
  struct store_data {
    runtime_context* ctx;
    wasmtime::Memory* memory;
    io_backend_interface* io; // may be nullptr if io_backend::none
  };

  // Info about an instantiated module (for cross-module lambda support)
  struct module_info {
    wasmtime::Instance instance;
    std::optional<wasmtime::Table> func_table;
    std::uint32_t lambda_count;
    std::filesystem::path source_file; // full path to source file, for relative imports
  };

  /// Get module info by ID. Used for cross-module lambda calls.
  [[nodiscard]] auto get_module(std::uint16_t module_id) const -> const module_info*;

  /// Get the current module ID (for closures created during execution)
  [[nodiscard]] auto current_module_id() const noexcept -> std::uint16_t {
    return current_module_id_;
  }

private:
  // wasmtime objects
  std::unique_ptr<wasmtime::Engine> engine_;
  std::unique_ptr<wasmtime::Store> store_;
  std::optional<wasmtime::Memory> memory_;
  std::optional<wasmtime::Instance> instance_;

  // Handle-based WASM memory access - single source of truth
  std::unique_ptr<wasm_memory> wasm_mem_;

  // our runtime context (uses wasm_mem_ for all memory access)
  runtime_context ctx_;

  // store data for callbacks
  store_data store_data_;

  // I/O backend (optional, based on compile-time config)
  std::unique_ptr<io_backend_interface> io_;

  // Cross-module lambda support:
  // Each instantiated module gets a unique ID (0 = parent, 1+ = imported modules)
  // The func_index in closures is encoded as: (module_id << 16) | func_index
  std::vector<module_info> modules_;
  std::uint16_t current_module_id_ = 0;

  // setup methods
  void setup_linker(wasmtime::Linker& linker);
  void setup_io_linker(wasmtime::Linker& linker);

  // Register a new module and return its ID
  auto register_module(wasmtime::Instance instance, std::filesystem::path source_file)
      -> std::uint16_t;
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

} // namespace straylight::nix::compiler::runtime
