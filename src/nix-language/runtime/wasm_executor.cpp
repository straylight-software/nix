// straylight // nix-language // runtime
//
// WASM executor implementation using wasmtime

#include "nix-language/runtime/wasm_executor.hh"

#include <any>
#include <cstring>
#include <sstream>

namespace nix::language::runtime {

// =============================================================================
// Constructor/Destructor
// =============================================================================

wasm_executor::wasm_executor() : engine_(std::make_unique<wasmtime::Engine>()) {
  // Store will be created fresh for each execution
}

wasm_executor::~wasm_executor() = default;

wasm_executor::wasm_executor(wasm_executor&&) noexcept = default;
auto wasm_executor::operator=(wasm_executor&&) noexcept -> wasm_executor& = default;

// =============================================================================
// Helper to get runtime context from caller
// =============================================================================

/// sync WASM memory to the runtime context
static void sync_to_ctx(wasmtime::Caller& caller, wasm_executor::store_data* data) {
  if (data->memory) {
    auto wasm_data = data->memory->data(caller.context());
    if (wasm_data.size() > data->ctx->memory.size()) {
      data->ctx->memory.resize(wasm_data.size());
    }
    std::memcpy(data->ctx->memory.data(), wasm_data.data(), wasm_data.size());
  }
}

/// sync runtime context memory back to WASM
static void sync_from_ctx(wasmtime::Caller& caller, wasm_executor::store_data* data) {
  if (data->memory) {
    auto wasm_data = data->memory->data(caller.context());
    auto copy_size = std::min(wasm_data.size(), data->ctx->memory.size());
    std::memcpy(wasm_data.data(), data->ctx->memory.data(), copy_size);
  }
}

static auto get_ctx(wasmtime::Caller& caller) -> runtime_context* {
  auto& data = caller.context().get_data();
  auto* sd = std::any_cast<wasm_executor::store_data*>(data);
  // Sync WASM memory to context on every access
  sync_to_ctx(caller, sd);
  return sd->ctx;
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
                     return rt_make_closure(*ctx, static_cast<std::uint32_t>(func_index),
                                            static_cast<std::uint32_t>(env_offset),
                                            static_cast<std::uint32_t>(env_size));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
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
                     return rt_make_thunk(*ctx, static_cast<std::uint32_t>(func_index),
                                          static_cast<std::uint32_t>(env_offset),
                                          static_cast<std::uint32_t>(env_size));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   }
                 })
      .unwrap();

  // ==========================================================================
  // Builtins module imports
  // ==========================================================================

  // __add(a: i64, b: i64, line: i32, col: i32) -> i64
  linker
      .func_wrap("builtins", "__add",
                 [](wasmtime::Caller caller, std::int64_t a, std::int64_t b, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_add(*ctx, a, b, static_cast<std::uint32_t>(line),
                                   static_cast<std::uint32_t>(col));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   }
                 })
      .unwrap();

  // __sub(a: i64, b: i64, line: i32, col: i32) -> i64
  linker
      .func_wrap("builtins", "__sub",
                 [](wasmtime::Caller caller, std::int64_t a, std::int64_t b, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_sub(*ctx, a, b, static_cast<std::uint32_t>(line),
                                   static_cast<std::uint32_t>(col));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   }
                 })
      .unwrap();

  // __mul(a: i64, b: i64, line: i32, col: i32) -> i64
  linker
      .func_wrap("builtins", "__mul",
                 [](wasmtime::Caller caller, std::int64_t a, std::int64_t b, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_mul(*ctx, a, b, static_cast<std::uint32_t>(line),
                                   static_cast<std::uint32_t>(col));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   }
                 })
      .unwrap();

  // __div(a: i64, b: i64, line: i32, col: i32) -> i64
  linker
      .func_wrap("builtins", "__div",
                 [](wasmtime::Caller caller, std::int64_t a, std::int64_t b, std::int32_t line,
                    std::int32_t col) -> wasmtime::Result<std::int64_t, wasmtime::Trap> {
                   try {
                     auto* ctx = get_ctx(caller);
                     return rt_div(*ctx, a, b, static_cast<std::uint32_t>(line),
                                   static_cast<std::uint32_t>(col));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
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
                     return rt_make_list(*ctx, static_cast<std::uint32_t>(offset),
                                         static_cast<std::uint32_t>(count));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
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
                     return rt_make_attrs(*ctx, static_cast<std::uint32_t>(offset),
                                          static_cast<std::uint32_t>(count));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
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
                     return rt_make_attrs_dynamic(*ctx, static_cast<std::uint32_t>(offset),
                                                  static_cast<std::uint32_t>(count));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
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
                     return rt_update(*ctx, a, b);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
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
                     return rt_concat(*ctx, a, b);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
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
                     return rt_to_string(*ctx, v);
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
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
                     return rt_concat_strings(*ctx, static_cast<std::uint32_t>(offset),
                                              static_cast<std::uint32_t>(count));
                   } catch (const runtime_error& e) {
                     return wasmtime::Trap(e.what());
                   }
                 })
      .unwrap();
}

// =============================================================================
// Execution
// =============================================================================

auto wasm_executor::execute(std::span<const std::uint8_t> wasm_binary) -> execution_result {
  try {
    // Create a fresh store for this execution
    store_ = std::make_unique<wasmtime::Store>(*engine_);

    // Will set store data after memory is created
    store_data_ = store_data{&ctx_, nullptr};

    // Initialize builtins (true, false, null, and the builtins attrset)
    ctx_.builtins["true"] = compile::packed::boolean_true;
    ctx_.builtins["false"] = compile::packed::boolean_false;
    ctx_.builtins["null"] = compile::packed::null_value;

    // Initialize the builtins attrset with all builtin functions
    rt_init_builtins(ctx_);

    // Create memory to provide to the module
    // Start with 1 page (64KB), allow growing to 256 pages (16MB)
    wasmtime::MemoryType mem_type(1, 256);
    auto mem_result = wasmtime::Memory::create(store_->context(), mem_type);
    if (!mem_result) {
      return execution_result::err("failed to create memory: " + mem_result.err().message());
    }
    memory_ = std::move(mem_result).ok();

    // Update store data with memory pointer and set it in the store
    store_data_.memory = &*memory_;
    store_->context().set_data(&store_data_);

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

    // Instantiate the module
    auto instance_result = linker.instantiate(store_->context(), module);
    if (!instance_result) {
      auto err = std::move(instance_result).err();
      return execution_result::err("instantiation failed: " + err.message());
    }
    instance_ = std::move(instance_result).ok();

    // Sync memory to our context
    sync_memory_to_context();

    // Get the function table for indirect calls
    auto table_export = instance_->get(store_->context(), "functions");
    std::optional<wasmtime::Table> func_table;
    if (table_export && std::holds_alternative<wasmtime::Table>(*table_export)) {
      func_table = std::get<wasmtime::Table>(*table_export);
    }

    // Get the lambda count global for computing thunk table indices
    // Thunks are stored at [lambda_count, lambda_count + thunk_count) in the function table
    auto lambda_count_export = instance_->get(store_->context(), "__lambda_count");
    if (lambda_count_export && std::holds_alternative<wasmtime::Global>(*lambda_count_export)) {
      auto global = std::get<wasmtime::Global>(*lambda_count_export);
      auto val = global.get(store_->context());
      ctx_.lambda_count = static_cast<std::uint32_t>(val.i32());
    }

    // Set up the callback for calling WASM functions from the runtime
    // This is needed for closure/thunk evaluation
    ctx_.call_wasm_func = [this, func_table](std::uint32_t func_index, std::uint32_t env_ptr,
                                             nix_value arg) -> nix_value {
      if (!func_table) {
        throw runtime_error("no function table available for indirect calls");
      }

      // Sync memory from context to WASM before the call
      sync_memory_from_context();

      // Get the function from the table
      auto val_opt = func_table->get(store_->context(), func_index);
      if (!val_opt) {
        throw runtime_error("function index " + std::to_string(func_index) + " out of bounds");
      }
      auto& val = *val_opt;
      auto func_opt = val.funcref();
      if (!func_opt) {
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
        throw runtime_error("indirect call failed: " + err.message());
      }
      auto results = std::move(call_result).ok();

      // Sync memory back to context after the call
      sync_memory_to_context();

      if (results.empty()) {
        throw runtime_error("function returned no value");
      }
      return results[0].i64();
    };

    // Set up the callback for calling thunk functions (single env_ptr argument)
    ctx_.call_wasm_thunk = [this, func_table](std::uint32_t func_index,
                                              std::uint32_t env_ptr) -> nix_value {
      if (!func_table) {
        throw runtime_error("no function table available for indirect calls");
      }

      // Sync memory from context to WASM before the call
      sync_memory_from_context();

      // Get the function from the table
      auto val_opt = func_table->get(store_->context(), func_index);
      if (!val_opt) {
        throw runtime_error("thunk index " + std::to_string(func_index) + " out of bounds");
      }
      auto& val = *val_opt;
      auto func_opt = val.funcref();
      if (!func_opt) {
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
        throw runtime_error("thunk call failed: " + err.message());
      }
      auto results = std::move(call_result).ok();

      // Sync memory back to context after the call
      sync_memory_to_context();

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

    // Sync memory back after execution
    if (memory_) {
      sync_memory_to_context();
    }

    // Extract the result
    if (results.empty()) {
      return execution_result::err("main returned no value");
    }

    // Force the result if it's a thunk (lazy evaluation requires final force)
    auto value = results[0].i64();
    value = rt_force(ctx_, value);

    // Sync memory after forcing (in case thunk evaluation modified memory)
    if (memory_) {
      sync_memory_to_context();
    }

    return execution_result::ok(value);

  } catch (const runtime_error& e) {
    return execution_result::err(e.what(), e.line, e.column);
  } catch (const std::exception& e) {
    return execution_result::err(e.what());
  }
}

// =============================================================================
// Memory Sync
// =============================================================================

void wasm_executor::sync_memory_to_context() {
  if (!memory_)
    return;

  auto data = memory_->data(store_->context());
  if (data.size() > ctx_.memory.size()) {
    ctx_.memory.resize(data.size());
  }
  std::memcpy(ctx_.memory.data(), data.data(), data.size());
}

void wasm_executor::sync_memory_from_context() {
  if (!memory_)
    return;

  auto data = memory_->data(store_->context());
  auto copy_size = std::min(data.size(), ctx_.memory.size());
  std::memcpy(data.data(), ctx_.memory.data(), copy_size);
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
      // Read 8 bytes as f64 (little-endian)
      auto bytes = ctx_.read_bytes(offset, 8);
      double d;
      std::memcpy(&d, bytes.data(), 8);
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
      auto count = get_payload(v);
      ss << "[" << count << " elements]";
      break;
    }

    case value_tag::attribute_set: {
      auto count = get_payload(v);
      ss << "{ " << count << " attrs }";
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

} // namespace nix::language::runtime
