// straylight // nix-language // runtime
//
// WASM executor implementation using wasmtime

#include "straylight/nix/compiler/runtime/wasm_executor.h"

#include <any>
#include <cstring>
#include <iostream>
#include <sstream>

#include "straylight/nix/compiler/runtime/io_backend.h"

namespace straylight::nix::compiler::runtime {

// =============================================================================
// Constructor/Destructor
// =============================================================================

wasm_executor::wasm_executor() : engine_(std::make_unique<wasmtime::Engine>()) {
  // Create default I/O backend based on compile-time config
  if constexpr (has_io()) {
    io_ = make_default_io_backend();
  }
}

wasm_executor::wasm_executor(std::unique_ptr<io_backend_interface> io)
    : engine_(std::make_unique<wasmtime::Engine>()), io_(std::move(io)) {
  // Store will be created fresh for each execution
}

wasm_executor::~wasm_executor() = default;

wasm_executor::wasm_executor(wasm_executor&&) noexcept = default;
auto wasm_executor::operator=(wasm_executor&&) noexcept -> wasm_executor& = default;

// =============================================================================
// Helper to get runtime context from caller
// =============================================================================

/// Get the runtime context from a wasmtime caller.
/// Since runtime_context now uses wasm_memory directly (which accesses WASM
/// memory through callbacks), there's no syncing needed.
static auto get_ctx([[maybe_unused]] wasmtime::Caller& caller) -> runtime_context* {
  auto& data = caller.context().get_data();
  auto* sd = std::any_cast<wasm_executor::store_data*>(data);
  return sd->ctx;
}

static auto get_io([[maybe_unused]] wasmtime::Caller& caller) -> io_backend_interface* {
  auto& data = caller.context().get_data();
  auto* sd = std::any_cast<wasm_executor::store_data*>(data);
  return sd->io;
}

// Helper template to wrap callbacks with exception handling.
// Converts C++ exceptions to wasmtime traps.
template <typename F>
auto wrap_callback(F&& f) {
  return
      [f = std::forward<F>(f)](auto&&... args)
          -> wasmtime::Result<decltype(f(std::forward<decltype(args)>(args)...)), wasmtime::Trap> {
        try {
          return f(std::forward<decltype(args)>(args)...);
        } catch (const runtime_error& e) {
          return wasmtime::Trap(e.what());
        } catch (const std::exception& e) {
          return wasmtime::Trap(std::string("runtime error: ") + e.what());
        }
      };
}

// =============================================================================
// Linker Setup
// =============================================================================

void wasm_executor::setup_io_linker(wasmtime::Linker& linker) {
  // I/O imports are only available when compiled with I/O support.
  // When compiled without I/O, these functions are not defined and
  // any WASM module that imports them will fail to instantiate.
  if constexpr (!has_io()) {
    return;
  }

  // ==========================================================================
  // I/O module imports - file system operations
  // ==========================================================================

  // __readFile(path_offset: i32) -> i64 (returns string value)
  linker
      .func_wrap("io", "__readFile",
                 [](wasmtime::Caller caller,
                    std::int32_t path_offset) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   auto* ctx = get_ctx(caller);
                   auto* io = get_io(caller);
                   if (!io) {
                     return wasmtime::Trap("I/O not available");
                   }

                   auto path = ctx->read_string(static_cast<std::uint32_t>(path_offset));
                   auto result = io->read_file(path);
                   if (!result) {
                     return wasmtime::Trap("readFile: " + std::string(path) + " failed");
                   }

                   // Allocate string in context memory and return string value
                   auto offset = ctx->alloc_string(result.value());
                   return make_value(compile::value_tag::string, offset);
                 })
      .unwrap();

  // __pathExists(path_offset: i32) -> i64 (returns bool value)
  linker
      .func_wrap("io", "__pathExists",
                 [](wasmtime::Caller caller,
                    std::int32_t path_offset) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   auto* ctx = get_ctx(caller);
                   auto* io = get_io(caller);
                   if (!io) {
                     return wasmtime::Trap("I/O not available");
                   }

                   auto path = ctx->read_string(static_cast<std::uint32_t>(path_offset));
                   bool exists = io->path_exists(path);
                   return exists ? compile::packed::boolean_true : compile::packed::boolean_false;
                 })
      .unwrap();

  // __readDir(path_offset: i32) -> i64 (returns attrset value)
  linker
      .func_wrap("io", "__readDir",
                 [](wasmtime::Caller caller,
                    std::int32_t path_offset) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   auto* ctx = get_ctx(caller);
                   auto* io = get_io(caller);
                   if (!io) {
                     return wasmtime::Trap("I/O not available");
                   }

                   auto path = ctx->read_string(static_cast<std::uint32_t>(path_offset));
                   auto result = io->read_dir(path);
                   if (!result) {
                     return wasmtime::Trap("readDir: " + std::string(path) + " failed");
                   }

                   // Build an attrset: { name → type_string, ... }
                   // Types: "regular", "directory", "symlink", "unknown"
                   const auto& entries = result.value();
                   auto count = static_cast<std::uint32_t>(entries.size());

                   // Allocate space for entries in memory (12 bytes each: key_offset + value)
                   auto entries_ptr = ctx->allocate(count * mem::ATTRSET_ENTRY_SIZE);

                   for (std::uint32_t idx = 0; idx < count; ++idx) {
                     const auto& entry = entries[idx];
                     auto name_offset = ctx->alloc_string(entry.name);
                     const char* type_str = "unknown";
                     switch (entry.type) {
                       case file_type::regular:
                         type_str = "regular";
                         break;
                       case file_type::directory:
                         type_str = "directory";
                         break;
                       case file_type::symlink:
                         type_str = "symlink";
                         break;
                       case file_type::unknown:
                         type_str = "unknown";
                         break;
                     }
                     auto type_offset = ctx->alloc_string(type_str);
                     auto type_val = make_value(compile::value_tag::string, type_offset);

                     // Write entry to memory
                     auto entry_offset = entries_ptr + idx * mem::ATTRSET_ENTRY_SIZE;
                     ctx->write_i32(entry_offset + mem::ATTRSET_ENTRY_KEY_OFFSET,
                                    static_cast<std::int32_t>(name_offset));
                     ctx->write_value(entry_offset + mem::ATTRSET_ENTRY_VALUE_OFFSET, type_val);
                   }

                   // Create the attrset
                   return rt_make_attrs(*ctx, entries_ptr, count);
                 })
      .unwrap();

  // __hashFile(algo_offset: i32, path_offset: i32) -> i64 (returns string value)
  linker
      .func_wrap("io", "__hashFile",
                 [](wasmtime::Caller caller, std::int32_t algo_offset,
                    std::int32_t path_offset) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   auto* ctx = get_ctx(caller);
                   auto* io = get_io(caller);
                   if (!io) {
                     return wasmtime::Trap("I/O not available");
                   }

                   auto algo = ctx->read_string(static_cast<std::uint32_t>(algo_offset));
                   auto path = ctx->read_string(static_cast<std::uint32_t>(path_offset));
                   auto result = io->hash_file(algo, path);
                   if (!result) {
                     return wasmtime::Trap("hashFile: " + std::string(path) + " failed");
                   }

                   auto offset = ctx->alloc_string(result.value());
                   return make_value(compile::value_tag::string, offset);
                 })
      .unwrap();

  // __import is handled via rt_apply_primop in runtime.cpp, not as a direct WASM import.
  // The import primop is applied like any other primop, and the runtime calls io->import_file().
}

void wasm_executor::setup_linker(wasmtime::Linker& linker) {
  // Runtime imports go in the "runtime" module
  // Builtin imports go in the "builtins" module

  // ==========================================================================
  // Runtime module imports
  // ==========================================================================

  // __throw(msg_offset: i32, line: i32, col: i32) -> i64 (never returns, always traps)
  linker
      .func_wrap(
          "runtime", "__throw",
          [](wasmtime::Caller caller, std::int32_t msg_offset, std::int32_t line,
             std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
            auto* ctx = get_ctx(caller);
            auto msg = ctx->read_string(static_cast<std::uint32_t>(msg_offset));
            std::string error_msg = std::string(msg);
            if (line != 0 || col != 0) {
              error_msg += " at line " + std::to_string(line) + ", column " + std::to_string(col);
            }
            // Store error in context for later retrieval
            ctx->set_error(msg, static_cast<std::uint32_t>(line), static_cast<std::uint32_t>(col));
            return wasmtime::Trap(error_msg);
          })
      .unwrap();

  // __force(value: i64) -> i64
  linker
      .func_wrap("runtime", "__force",
                 [](wasmtime::Caller caller,
                    std::int64_t value) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_force(*ctx, value);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __expectBool(value: i64, line: i32, col: i32) -> i64
  linker
      .func_wrap("runtime", "__expectBool",
                 [](wasmtime::Caller caller, std::int64_t value, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_expect_bool(*ctx, value, static_cast<std::uint32_t>(line),
                                           static_cast<std::uint32_t>(col));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __apply(fn: i64, arg: i64) -> i64
  linker
      .func_wrap("runtime", "__apply",
                 [](wasmtime::Caller caller, std::int64_t fn,
                    std::int64_t arg) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_apply(*ctx, fn, arg);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __lookupVar(name_offset: i32) -> i64
  linker
      .func_wrap("runtime", "__lookupVar",
                 [](wasmtime::Caller caller,
                    std::int32_t name_offset) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_lookup_var(*ctx, static_cast<std::uint32_t>(name_offset));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __makeClosure(func_index: i32, env_offset: i32, env_size: i32) -> i64
  linker
      .func_wrap("runtime", "__makeClosure",
                 [](wasmtime::Caller caller, std::int32_t func_index, std::int32_t env_offset,
                    std::int32_t env_size) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_make_closure(*ctx, static_cast<std::uint32_t>(func_index),
                                                   static_cast<std::uint32_t>(env_offset),
                                                   static_cast<std::uint32_t>(env_size));


                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __makeThunk(func_index: i32, env_offset: i32, env_size: i32) -> i64
  linker
      .func_wrap("runtime", "__makeThunk",
                 [](wasmtime::Caller caller, std::int32_t func_index, std::int32_t env_offset,
                    std::int32_t env_size) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_make_thunk(*ctx, static_cast<std::uint32_t>(func_index),
                                                 static_cast<std::uint32_t>(env_offset),
                                                 static_cast<std::uint32_t>(env_size));


                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // ==========================================================================
  // Builtins module imports
  // ==========================================================================

  // __add(a: i64, b: i64, line: i32, col: i32) -> i64
  // Note: rt_add can allocate memory for string concatenation or floats
  linker
      .func_wrap("builtins", "__add",
                 [](wasmtime::Caller caller, std::int64_t a, std::int64_t b, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_add(*ctx, a, b, static_cast<std::uint32_t>(line),
                                          static_cast<std::uint32_t>(col));

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __sub(a: i64, b: i64, line: i32, col: i32) -> i64
  // Note: can allocate for float results
  linker
      .func_wrap("builtins", "__sub",
                 [](wasmtime::Caller caller, std::int64_t a, std::int64_t b, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_sub(*ctx, a, b, static_cast<std::uint32_t>(line),
                                          static_cast<std::uint32_t>(col));

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __mul(a: i64, b: i64, line: i32, col: i32) -> i64
  // Note: can allocate for float results
  linker
      .func_wrap("builtins", "__mul",
                 [](wasmtime::Caller caller, std::int64_t a, std::int64_t b, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_mul(*ctx, a, b, static_cast<std::uint32_t>(line),
                                          static_cast<std::uint32_t>(col));

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __div(a: i64, b: i64, line: i32, col: i32) -> i64
  // Note: can allocate for float results
  linker
      .func_wrap("builtins", "__div",
                 [](wasmtime::Caller caller, std::int64_t a, std::int64_t b, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_div(*ctx, a, b, static_cast<std::uint32_t>(line),
                                          static_cast<std::uint32_t>(col));

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __negate(v: i64) -> i64
  linker
      .func_wrap("builtins", "__negate",
                 [](wasmtime::Caller caller,
                    std::int64_t v) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_negate(*ctx, v);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __lessThan(a: i64, b: i64) -> i64
  linker
      .func_wrap("builtins", "__lessThan",
                 [](wasmtime::Caller caller, std::int64_t a,
                    std::int64_t b) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_less_than(*ctx, a, b);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __lessEq(a: i64, b: i64) -> i64
  linker
      .func_wrap("builtins", "__lessEq",
                 [](wasmtime::Caller caller, std::int64_t a,
                    std::int64_t b) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_less_eq(*ctx, a, b);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __eq(a: i64, b: i64) -> i64
  linker
      .func_wrap("builtins", "__eq",
                 [](wasmtime::Caller caller, std::int64_t a,
                    std::int64_t b) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_eq(*ctx, a, b);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __neq(a: i64, b: i64) -> i64
  linker
      .func_wrap("builtins", "__neq",
                 [](wasmtime::Caller caller, std::int64_t a,
                    std::int64_t b) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_neq(*ctx, a, b);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __not(v: i64) -> i64
  linker
      .func_wrap("builtins", "__not",
                 [](wasmtime::Caller caller,
                    std::int64_t v) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_not(*ctx, v);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __isBool(v: i64) -> i32
  linker
      .func_wrap("builtins", "__isBool",
                 [](wasmtime::Caller caller,
                    std::int64_t v) -> wasmtime::Result<std::int32_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_is_bool(*ctx, v);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __makeList(offset: i32, count: i32) -> i64
  linker
      .func_wrap("builtins", "__makeList",
                 [](wasmtime::Caller caller, std::int32_t offset,
                    std::int32_t count) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_make_list(*ctx, static_cast<std::uint32_t>(offset),
                                                static_cast<std::uint32_t>(count));

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __makeAttrs(offset: i32, count: i32) -> i64
  linker
      .func_wrap("builtins", "__makeAttrs",
                 [](wasmtime::Caller caller, std::int32_t offset,
                    std::int32_t count) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_make_attrs(*ctx, static_cast<std::uint32_t>(offset),
                                                 static_cast<std::uint32_t>(count));

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __makeAttrsDynamic(offset: i32, count: i32) -> i64
  linker
      .func_wrap("builtins", "__makeAttrsDynamic",
                 [](wasmtime::Caller caller, std::int32_t offset,
                    std::int32_t count) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_make_attrs_dynamic(*ctx, static_cast<std::uint32_t>(offset),
                                                         static_cast<std::uint32_t>(count));

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __select(set: i64, key_offset: i32, line: i32, col: i32) -> i64
  linker
      .func_wrap(
          "builtins", "__select",
          [](wasmtime::Caller caller, std::int64_t set, std::int32_t key_offset, std::int32_t line,
             std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
            try {
              auto* ctx = get_ctx(caller);
              return rt_select(*ctx, set, static_cast<std::uint32_t>(key_offset),
                               static_cast<std::uint32_t>(line), static_cast<std::uint32_t>(col));
            } catch (const runtime_error& e) {
              return wasmtime::Trap(e.what());
            } catch (const std::exception& e) {
              return wasmtime::Trap(std::string("runtime error: ") + e.what());
            }
          })
      .unwrap();

  // __selectDynamic(set: i64, key: i64, line: i32, col: i32) -> i64
  linker
      .func_wrap("builtins", "__selectDynamic",
                 [](wasmtime::Caller caller, std::int64_t set, std::int64_t key, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_select_dynamic(*ctx, set, key, static_cast<std::uint32_t>(line),
                                              static_cast<std::uint32_t>(col));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __hasAttr(set: i64, key_offset: i32) -> i64
  linker
      .func_wrap("builtins", "__hasAttr",
                 [](wasmtime::Caller caller, std::int64_t set,
                    std::int32_t key_offset) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_has_attr(*ctx, set, static_cast<std::uint32_t>(key_offset));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __hasAttrDynamic(set: i64, key: i64) -> i64
  linker
      .func_wrap("builtins", "__hasAttrDynamic",
                 [](wasmtime::Caller caller, std::int64_t set,
                    std::int64_t key) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_has_attr_dynamic(*ctx, set, key);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __update(a: i64, b: i64) -> i64
  linker
      .func_wrap("builtins", "__update",
                 [](wasmtime::Caller caller, std::int64_t a,
                    std::int64_t b) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_update(*ctx, a, b);

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __concat(a: i64, b: i64) -> i64
  linker
      .func_wrap("builtins", "__concat",
                 [](wasmtime::Caller caller, std::int64_t a,
                    std::int64_t b) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_concat(*ctx, a, b);

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __toString(v: i64) -> i64
  linker
      .func_wrap("builtins", "__toString",
                 [](wasmtime::Caller caller,
                    std::int64_t v) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_to_string(*ctx, v);

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();

  // __concatStrings(offset: i32, count: i32) -> i64
  linker
      .func_wrap("builtins", "__concatStrings",
                 [](wasmtime::Caller caller, std::int32_t offset,
                    std::int32_t count) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     auto result = rt_concat_strings(*ctx, static_cast<std::uint32_t>(offset),
                                                     static_cast<std::uint32_t>(count));

                     return result;
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   } catch (const std::exception& e) {
                     return wasmtime::Trap(std::string("runtime error: ") + e.what());
                   }
                 })
      .unwrap();
}

// =============================================================================
// Execution
// =============================================================================

auto wasm_executor::register_module(wasmtime::Instance instance) -> std::uint16_t {
  auto module_id = static_cast<std::uint16_t>(modules_.size());

  // Get function table
  auto table_export = instance.get(store_->context(), "functions");
  std::optional<wasmtime::Table> func_table;
  if (table_export && std::holds_alternative<wasmtime::Table>(*table_export)) {
    func_table = std::get<wasmtime::Table>(*table_export);
  }

  // Get lambda count
  std::uint32_t lambda_count = 0;
  auto lambda_count_export = instance.get(store_->context(), "__lambda_count");
  if (lambda_count_export && std::holds_alternative<wasmtime::Global>(*lambda_count_export)) {
    auto global = std::get<wasmtime::Global>(*lambda_count_export);
    auto val = global.get(store_->context());
    lambda_count = static_cast<std::uint32_t>(val.i32());
  }

  modules_.push_back(module_info{
      .instance = std::move(instance),
      .func_table = std::move(func_table),
      .lambda_count = lambda_count,
  });

  return module_id;
}

auto wasm_executor::get_module(std::uint16_t module_id) const -> const module_info* {
  if (module_id >= modules_.size()) {
    return nullptr;
  }
  return &modules_[module_id];
}

auto wasm_executor::execute(std::span<const std::uint8_t> wasm_binary) -> execution_result {
  try {
    // Create a fresh store for this execution
    store_ = std::make_unique<wasmtime::Store>(*engine_);

    // Clear module registry for fresh execution
    modules_.clear();
    current_module_id_ = 0;

    // Set the I/O backend in the context so rt_apply_primop can access it
    ctx_.io = io_.get();

    // Will set store data after memory is created
    store_data_ = store_data{&ctx_, nullptr, io_.get()};

    // Initialize primitive builtins (true, false, null, import)
    // These don't allocate memory so can be done before WASM memory exists
    ctx_.builtins["true"] = compile::packed::boolean_true;
    ctx_.builtins["false"] = compile::packed::boolean_false;
    ctx_.builtins["null"] = compile::packed::null_value;
    // import is both a top-level builtin and available as builtins.import
    ctx_.builtins["import"] =
        make_value(compile::value_tag::primop, compile::builtins::import_path);

    // Create memory to provide to the module
    // Start with 16 pages (1MB) to cover heap at HEAP_BASE (0x20000 = 128KB)
    // Allow growing to 256 pages (16MB)
    wasmtime::MemoryType mem_type(16, 256);
    auto mem_result = wasmtime::Memory::create(store_->context(), mem_type);
    if (!mem_result) {
      return execution_result::err("failed to create memory: " + mem_result.err().message());
    }
    memory_ = std::move(mem_result).ok();

    // Update store data with memory pointer and set it in the store
    store_data_.memory = &*memory_;
    store_->context().set_data(&store_data_);

    // Create wasm_memory - the single source of truth for all memory access.
    // No more dual buffers, no more syncing.
    wasm_mem_ = std::make_unique<wasm_memory>(
        // get_memory callback - always fetches current WASM memory span
        [this]() -> std::span<std::uint8_t> { return memory_->data(store_->context()); },
        // grow_memory callback
        [this](std::uint32_t pages) -> bool {
          auto result = memory_->grow(store_->context(), pages);
          return static_cast<bool>(result);
        },
        mem::HEAP_BASE);

    // Connect runtime_context to wasm_memory
    ctx_.mem = wasm_mem_.get();

    // Make a mutable copy of the wasm binary (wasmtime::Module::compile needs non-const)
    std::vector<std::uint8_t> wasm_copy(wasm_binary.begin(), wasm_binary.end());

    // Compile the module
    auto module_result = wasmtime::Module::compile(*engine_, wasm_copy);
    if (!module_result) {
      return execution_result::err("failed to compile WASM module: " +
                                   module_result.err().message());
    }
    auto module = std::move(module_result).ok();

    // Create linker and setup imports
    wasmtime::Linker linker(*engine_);

    // Define memory in the linker (module imports memory from "env")
    linker.define(store_->context(), "env", "memory", *memory_).unwrap();

    // Setup function imports
    setup_linker(linker);

    // Setup I/O imports (conditionally compiled)
    setup_io_linker(linker);

    // Instantiate the module
    auto instance_result = linker.instantiate(store_->context(), module);
    if (!instance_result) {
      auto err = std::move(instance_result).err();
      return execution_result::err("instantiation failed: " + err.message());
    }
    auto instance = std::move(instance_result).ok();

    // Save instance before registering (register_module moves it)
    instance_ = instance;

    // Register the parent module (ID 0)
    current_module_id_ = register_module(std::move(instance));

    // Set current module ID in context for closure creation
    ctx_.current_module_id = current_module_id_;

    // Initialize the builtins attrset with all builtin functions.
    // Now that we use wasm_memory directly, no syncing needed - it writes
    // directly to WASM linear memory.
    rt_init_builtins(ctx_);

    // Get lambda count from the registered module
    ctx_.lambda_count = modules_[current_module_id_].lambda_count;

    // Set up the callback for calling WASM functions from the runtime
    // This callback handles cross-module lambda calls by decoding the module_id
    ctx_.call_wasm_func = [this](std::uint32_t encoded_func_index, std::uint32_t env_ptr,
                                 nix_value arg) -> nix_value {
      // Decode module_id and local func_index
      auto module_id = static_cast<std::uint16_t>(encoded_func_index >> 16);
      auto local_func_index = encoded_func_index & 0xFFFF;

      auto* mod = get_module(module_id);
      if (!mod || !mod->func_table) {
        throw runtime_error("module " + std::to_string(module_id) +
                            " not found or has no function table");
      }

      // Save and switch current_module_id so closures created during this call
      // get the correct module_id encoded
      auto saved_module_id = ctx_.current_module_id;
      ctx_.current_module_id = module_id;

      // Get the function from the module's table
      auto val_opt = mod->func_table->get(store_->context(), local_func_index);
      if (!val_opt) {
        ctx_.current_module_id = saved_module_id;
        throw runtime_error("function index " + std::to_string(local_func_index) +
                            " out of bounds in module " + std::to_string(module_id));
      }
      auto& val = *val_opt;
      auto func_opt = val.funcref();
      if (!func_opt) {
        ctx_.current_module_id = saved_module_id;
        throw runtime_error("table entry is not a function (null funcref)");
      }
      auto func = *func_opt;

      // Lambda functions have signature (env_ptr: i32, arg: i64) -> i64
      std::vector<wasmtime::Val> params{
          wasmtime::Val(static_cast<std::int32_t>(env_ptr)),
          wasmtime::Val(static_cast<std::int64_t>(arg)),
      };

      auto call_result = func.call(store_->context(), params);
      if (!call_result) {
        auto err = std::move(call_result).err();
        ctx_.current_module_id = saved_module_id;
        throw runtime_error("indirect call failed: " + err.message());
      }
      auto results = std::move(call_result).ok();

      // Restore the original module_id
      ctx_.current_module_id = saved_module_id;

      if (results.empty()) {
        throw runtime_error("function returned no value");
      }
      return results[0].i64();
    };

    // Set up the callback for calling thunk functions (single env_ptr argument)
    ctx_.call_wasm_thunk = [this](std::uint32_t encoded_func_index,
                                  std::uint32_t env_ptr) -> nix_value {
      // Decode module_id and local thunk index
      // Format: (module_id << 16) | local_thunk_index
      // local_thunk_index is the thunk's index (0, 1, 2...) within that module's thunks
      auto module_id = static_cast<std::uint16_t>(encoded_func_index >> 16);
      auto local_thunk_index = encoded_func_index & 0xFFFF;

      auto* mod = get_module(module_id);
      if (!mod || !mod->func_table) {
        throw runtime_error("module " + std::to_string(module_id) +
                            " not found or has no function table for thunk");
      }

      // Thunks are stored at [lambda_count, lambda_count + thunk_count) in the function table
      // Add the module-specific lambda_count to get the actual table index
      auto table_index = local_thunk_index + mod->lambda_count;

      // Save and switch current_module_id so closures created during this call
      // get the correct module_id encoded
      auto saved_module_id = ctx_.current_module_id;
      ctx_.current_module_id = module_id;

      // Get the function from the module's table
      auto val_opt = mod->func_table->get(store_->context(), table_index);
      if (!val_opt) {
        ctx_.current_module_id = saved_module_id;
        throw runtime_error("thunk index " + std::to_string(table_index) +
                            " (local=" + std::to_string(local_thunk_index) +
                            ", lambda_count=" + std::to_string(mod->lambda_count) +
                            ") out of bounds in module " + std::to_string(module_id));
      }
      auto& val = *val_opt;
      auto func_opt = val.funcref();
      if (!func_opt) {
        ctx_.current_module_id = saved_module_id;
        throw runtime_error("table entry is not a function (null funcref)");
      }
      auto func = *func_opt;

      // Thunk functions have signature (env_ptr: i32) -> i64
      std::vector<wasmtime::Val> params{
          wasmtime::Val(static_cast<std::int32_t>(env_ptr)),
      };

      auto call_result = func.call(store_->context(), params);
      if (!call_result) {
        auto err = std::move(call_result).err();
        ctx_.current_module_id = saved_module_id;
        throw runtime_error("thunk call failed: " + err.message());
      }
      auto results = std::move(call_result).ok();

      // Restore the original module_id
      ctx_.current_module_id = saved_module_id;

      if (results.empty()) {
        throw runtime_error("thunk returned no value");
      }
      return results[0].i64();
    };

    // Get the main function
    auto main_export = instance_->get(store_->context(), "main");
    if (!main_export) {
      return execution_result::err("module has no 'main' export");
    }
    if (!std::holds_alternative<wasmtime::Func>(*main_export)) {
      return execution_result::err("'main' export is not a function");
    }
    auto main_func = std::get<wasmtime::Func>(*main_export);

    // Call main() - pass empty params, returns vector of Val
    std::vector<wasmtime::Val> params;
    auto call_result = main_func.call(store_->context(), params);
    if (!call_result) {
      auto err = std::move(call_result).err();
      return execution_result::err("call failed: " + err.message());
    }
    auto results = std::move(call_result).ok();

    // Extract the result
    if (results.empty()) {
      return execution_result::err("main returned no value");
    }

    // Force the result if it's a thunk (lazy evaluation requires final force)
    auto value = results[0].i64();
    value = rt_force(ctx_, value);

    return execution_result::ok(value);

  } catch (const runtime_error& e) {
    return execution_result::err(e.what(), e.line, e.column);
  } catch (const std::exception& e) {
    return execution_result::err(e.what());
  }
}

auto wasm_executor::execute_within(std::span<const std::uint8_t> wasm_binary) -> execution_result {
  // This method executes a WASM module within the existing context.
  // It's used for `import` to share memory with the parent module.
  // Prerequisites: must be called during an existing execute() call.

  if (!store_ || !memory_) {
    return execution_result::err("execute_within called without active context");
  }

  try {
    // Make a mutable copy of the wasm binary
    std::vector<std::uint8_t> wasm_copy(wasm_binary.begin(), wasm_binary.end());

    // Compile the module
    auto module_result = wasmtime::Module::compile(*engine_, wasm_copy);
    if (!module_result) {
      return execution_result::err("failed to compile WASM module: " +
                                   module_result.err().message());
    }
    auto module = std::move(module_result).ok();

    // Create linker and setup imports (reusing the same setup as execute())
    wasmtime::Linker linker(*engine_);

    // Define memory in the linker (share the existing memory)
    linker.define(store_->context(), "env", "memory", *memory_).unwrap();

    // Setup function imports
    setup_linker(linker);

    // Setup I/O imports
    setup_io_linker(linker);

    // Instantiate the module
    auto instance_result = linker.instantiate(store_->context(), module);
    if (!instance_result) {
      auto err = std::move(instance_result).err();
      return execution_result::err("instantiation failed: " + err.message());
    }
    auto instance = std::move(instance_result).ok();

    // Get the main function BEFORE registering (which moves the instance)
    auto main_export = instance.get(store_->context(), "main");
    if (!main_export) {
      return execution_result::err("module has no 'main' export");
    }
    if (!std::holds_alternative<wasmtime::Func>(*main_export)) {
      return execution_result::err("'main' export is not a function");
    }
    auto main_func = std::get<wasmtime::Func>(*main_export);

    // Register this child module and get its ID
    // Save the parent's module ID to restore after execution
    auto parent_module_id = current_module_id_;
    current_module_id_ = register_module(std::move(instance));
    ctx_.current_module_id = current_module_id_;

    // Call main()
    std::vector<wasmtime::Val> params;
    auto call_result = main_func.call(store_->context(), params);
    if (!call_result) {
      auto err = std::move(call_result).err();
      current_module_id_ = parent_module_id;
      ctx_.current_module_id = parent_module_id;
      return execution_result::err("call failed: " + err.message());
    }
    auto results = std::move(call_result).ok();

    // Extract the result
    if (results.empty()) {
      current_module_id_ = parent_module_id;
      ctx_.current_module_id = parent_module_id;
      return execution_result::err("main returned no value");
    }

    // Force the result (while still in child module context)
    auto value = results[0].i64();
    value = rt_force(ctx_, value);

    // CRITICAL: Reify the value before returning.
    // Each module's data segment initialization overwrites the previous one,
    // so any strings (including attrset keys) that point to the data segment
    // will become invalid when the next module is imported.
    // Reification copies all data segment strings to the heap.
    value = rt_reify_value(ctx_, value);

    // Restore parent module ID
    current_module_id_ = parent_module_id;
    ctx_.current_module_id = parent_module_id;

    return execution_result::ok(value);

  } catch (const runtime_error& e) {
    return execution_result::err(e.what(), e.line, e.column);
  } catch (const std::exception& e) {
    return execution_result::err(e.what());
  }
}

// =============================================================================
// Value Formatting
// =============================================================================

auto wasm_executor::read_string_value(nix_value v) const -> std::string {
  if (!is_string(v)) {
    throw runtime_error("expected string value");
  }
  auto offset = get_payload(v);
  return std::string(ctx_.read_string(offset));
}

auto wasm_executor::format_value(nix_value v) const -> std::string {
  std::ostringstream ss;

  switch (get_tag(v)) {
    case value_tag::null_value:
      ss << "null";
      break;

    case value_tag::boolean:
      ss << (get_bool_value(v) ? "true" : "false");
      break;

    case value_tag::integer:
      ss << get_int_value(v);
      break;

    case value_tag::floating: {
      // payload is a pointer to f64 in memory
      auto offset = get_payload(v);
      auto d = ctx_.read_f64(offset);
      ss << d;
      break;
    }

    case value_tag::string: {
      auto offset = get_payload(v);
      ss << "\"" << ctx_.read_string(offset) << "\"";
      break;
    }

    case value_tag::path: {
      auto offset = get_payload(v);
      ss << ctx_.read_string(offset);
      break;
    }

    case value_tag::list: {
      auto ptr = get_payload(v);
      if (ptr == 0) {
        ss << "[ ]";
      } else {
        auto count = ctx_.read_u32(ptr + mem::LIST_COUNT_OFFSET);
        ss << "[" << count << " elements]";
      }
      break;
    }

    case value_tag::attribute_set: {
      auto ptr = get_payload(v);
      if (ptr == 0) {
        ss << "{ }";
      } else {
        auto count = ctx_.read_u32(ptr + mem::ATTRSET_COUNT_OFFSET);
        ss << "{ " << count << " attrs }";
      }
      break;
    }

    case value_tag::lambda:
      ss << "<lambda>";
      break;

    case value_tag::thunk:
      ss << "<thunk>";
      break;

    case value_tag::primop:
      ss << "<primop>";
      break;

    default:
      ss << "<unknown:" << static_cast<int>(get_tag(v)) << ">";
      break;
  }

  return ss.str();
}

} // namespace straylight::nix::compiler::runtime
