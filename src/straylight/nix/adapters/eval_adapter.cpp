// eval_adapter.cpp - Straylight evaluator adapter for nix CLI

#include "eval_adapter.h"

#include "straylight/nix/compiler/evaluator.h"
#include "straylight/nix/core/component.h"

#include "nix/expr/eval.h"
#include "nix/expr/value.h"
#include "nix/store/store-api.h"

namespace straylight::nix::adapters {

namespace {

// Convert straylight eval error to adapter error
auto convert_error(const compiler::eval_error& err) -> eval_error_info {
  eval_adapter_error kind;
  switch (err.kind) {
    case compiler::eval_error_kind::parse_error:
      kind = eval_adapter_error::parse_error;
      break;
    case compiler::eval_error_kind::compile_error:
      kind = eval_adapter_error::compile_error;
      break;
    case compiler::eval_error_kind::runtime_error:
      kind = eval_adapter_error::runtime_error;
      break;
    case compiler::eval_error_kind::io_error:
      kind = eval_adapter_error::io_error;
      break;
  }
  return eval_error_info{
      .kind = kind,
      .message = err.message,
      .file = err.file,
      .line = err.line,
      .column = err.column,
  };
}

} // namespace

// ============================================================================
// Construction
// ============================================================================

eval_adapter::eval_adapter(::nix::ref<::nix::store_t> store)
    : impl_(std::make_unique<compiler::evaluator>()), store_(store.get_ptr()) {}

eval_adapter::eval_adapter() : impl_(std::make_unique<compiler::evaluator>()) {}

eval_adapter::~eval_adapter() = default;

eval_adapter::eval_adapter(eval_adapter&&) noexcept = default;
eval_adapter& eval_adapter::operator=(eval_adapter&&) noexcept = default;

// ============================================================================
// String evaluation
// ============================================================================

auto eval_adapter::eval_string(std::string_view expr) -> eval_result<std::string> {
  auto result = impl_->eval_string(expr);
  if (!result) {
    return std::unexpected(convert_error(result.error()));
  }
  return *result;
}

auto eval_adapter::eval_file(const std::filesystem::path& path) -> eval_result<std::string> {
  auto result = impl_->eval_file(path);
  if (!result) {
    return std::unexpected(convert_error(result.error()));
  }
  return *result;
}

// ============================================================================
// JSON evaluation
// ============================================================================

auto eval_adapter::eval_to_json(std::string_view expr) -> eval_result<std::string> {
  // TODO: Implement proper JSON serialization
  // For now, just return the formatted value (which may not be valid JSON)
  return eval_string(expr);
}

auto eval_adapter::eval_file_to_json(const std::filesystem::path& path)
    -> eval_result<std::string> {
  // TODO: Implement proper JSON serialization
  return eval_file(path);
}

// ============================================================================
// Raw evaluation
// ============================================================================

auto eval_adapter::eval_raw(std::string_view expr) -> eval_result<std::int64_t> {
  auto result = impl_->eval_string_raw(expr);
  if (!result) {
    return std::unexpected(convert_error(result.error()));
  }
  return *result;
}

auto eval_adapter::eval_file_raw(const std::filesystem::path& path) -> eval_result<std::int64_t> {
  auto result = impl_->eval_file_raw(path);
  if (!result) {
    return std::unexpected(convert_error(result.error()));
  }
  return *result;
}

// ============================================================================
// Nix value interop
// ============================================================================

auto eval_adapter::eval_to_nix_value(std::string_view expr, ::nix::eval_state_t& state,
                                     ::nix::value_t& result) -> eval_result<bool> {
  // Evaluate the expression
  auto raw = eval_raw(expr);
  if (!raw) {
    return std::unexpected(raw.error());
  }

  // The straylight value is a 64-bit tagged pointer.
  // We need to decode it and convert to nix's value representation.
  //
  // Straylight value encoding (from compile/wasm_types.h):
  //   - Format: (payload << 32) | tag
  //   - Tags: 0=null, 1=bool, 2=int, 3=float, 4=string, 5=path,
  //           6=list, 7=attrset, 8=lambda, 9=thunk, 10=primop
  //
  // For now, we only support simple types (int, bool, null).
  // Full interop would require access to straylight's WASM memory to decode heap objects.

  std::int64_t raw_value = *raw;
  auto tag = static_cast<std::uint8_t>(raw_value & 0xFF);
  auto payload = static_cast<std::uint32_t>(raw_value >> 32);

  switch (tag) {
    case 0: // null
      result.mkNull();
      return true;
    case 1: // boolean
      result.mkBool(payload != 0);
      return true;
    case 2: // integer
      result.mkInt(::nix::NixInt{static_cast<std::int64_t>(payload)});
      return true;
    default:
      // Other types (float, string, path, list, attrset, lambda, etc.)
      // would require access to straylight's WASM memory
      return std::unexpected(eval_error_info{
          .kind = eval_adapter_error::conversion_error,
          .message = "conversion to nix value only supported for int/bool/null",
          .file = "<expr>",
          .line = 0,
          .column = 0,
      });
  }
}

// ============================================================================
// Query
// ============================================================================

bool eval_adapter::has_store() const noexcept {
  return store_ != nullptr;
}

// ============================================================================
// Factory
// ============================================================================

auto make_eval_adapter(::nix::ref<::nix::store_t> store) -> std::unique_ptr<eval_adapter> {
  return std::make_unique<eval_adapter>(store);
}

auto make_eval_adapter() -> std::unique_ptr<eval_adapter> {
  return std::make_unique<eval_adapter>();
}

auto try_straylight_eval(std::string_view expr) -> eval_result<std::string> {
  // Compile-time check if straylight evaluator is enabled
  if constexpr (!core::use_straylight_eval) {
    return std::unexpected(eval_error_info{
        .kind = eval_adapter_error::runtime_error,
        .message = "straylight evaluator not enabled (compile with -DSTRAYLIGHT_EVAL=1)",
        .file = "",
        .line = 0,
        .column = 0,
    });
  } else {
    eval_adapter adapter;
    return adapter.eval_string(expr);
  }
}

} // namespace straylight::nix::adapters
