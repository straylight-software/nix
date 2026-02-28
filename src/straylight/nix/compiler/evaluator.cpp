// straylight // nix-language
//
// Full Nix Evaluator Implementation

#include "straylight/nix/compiler/evaluator.h"

#include <cstring>
#include <fstream>
#include <iterator>

#include "straylight/nix/compiler/ast/symbol_table.h"
#include "straylight/nix/compiler/compile/compiler.h"
#include "straylight/nix/compiler/parse/parser.h"
#include "straylight/nix/compiler/runtime/io_backend.h"
#include "straylight/nix/compiler/runtime/wasm_executor.h"

namespace straylight::nix::compiler {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor/Destructor
// ─────────────────────────────────────────────────────────────────────────────

evaluator::evaluator() : executor_(std::make_unique<runtime::wasm_executor>()) {
  // Set up the import callback if I/O is enabled
  if constexpr (runtime::has_io()) {
    if (auto* io = executor_->io(); io != nullptr) {
      io->set_import_callback([this](std::string_view source, std::string_view path) {
        return eval_source(source, path);
      });
    }
  }
}

evaluator::evaluator(std::unique_ptr<runtime::io_backend_interface> io)
    : executor_(std::make_unique<runtime::wasm_executor>(std::move(io))) {
  // Set up the import callback
  if (auto* io_ptr = executor_->io(); io_ptr != nullptr) {
    io_ptr->set_import_callback([this](std::string_view source, std::string_view path) {
      return eval_source(source, path);
    });
  }
}

evaluator::~evaluator() = default;

evaluator::evaluator(evaluator&&) noexcept = default;
auto evaluator::operator=(evaluator&&) noexcept -> evaluator& = default;

// ─────────────────────────────────────────────────────────────────────────────
// Compilation
// ─────────────────────────────────────────────────────────────────────────────

auto evaluator::compile_source(std::string_view source, std::string_view path)
    -> eval_result<std::vector<std::uint8_t>> {
  try {
    // 1. Parse
    ast::symbol_table symbols;
    auto ast_result = parse::parse(source, symbols, std::filesystem::path(path).parent_path());

    // 2. Compile with the current data segment offset.
    // Each module's data segment starts after the previous one to avoid collisions
    // when sharing memory between parent and imported modules.
    compile::compiler comp(symbols, next_data_segment_offset_);
    auto wasm_module = comp.compile(ast_result);

    // Update the next data segment offset for future modules
    next_data_segment_offset_ = comp.data_segment_end();

    auto wasm_result = wasm_module.emit_binary();

    if (wasm_result.empty()) {
      return std::unexpected(eval_error{
          .kind = eval_error_kind::compile_error,
          .message = "compilation failed",
          .file = std::string(path),
          .line = 0,
          .column = 0,
      });
    }

    return wasm_result;

  } catch (const parse::parse_error& e) {
    return std::unexpected(eval_error{
        .kind = eval_error_kind::parse_error,
        .message = e.what(),
        .file = std::string(path),
        .line = e.position.line_,
        .column = e.position.column_,
    });
  } catch (const std::exception& e) {
    return std::unexpected(eval_error{
        .kind = eval_error_kind::compile_error,
        .message = e.what(),
        .file = std::string(path),
        .line = 0,
        .column = 0,
    });
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Evaluation
// ─────────────────────────────────────────────────────────────────────────────

auto evaluator::eval_string_raw(std::string_view source) -> eval_result<compile::nix_value> {
  // Compile
  auto wasm = compile_source(source, "<string>");
  if (!wasm) {
    return std::unexpected(wasm.error());
  }

  // Execute
  auto result = executor_->execute(*wasm);
  if (!result.success) {
    return std::unexpected(eval_error{
        .kind = eval_error_kind::runtime_error,
        .message = result.error,
        .file = "<string>",
        .line = result.line,
        .column = result.column,
    });
  }

  return result.value;
}

auto evaluator::eval_string(std::string_view source) -> eval_result<std::string> {
  auto value = eval_string_raw(source);
  if (!value) {
    return std::unexpected(value.error());
  }

  return executor_->format_value(*value);
}

auto evaluator::eval_file_raw(const std::filesystem::path& path)
    -> eval_result<compile::nix_value> {
  // Read the file
  std::ifstream file(path);
  if (!file) {
    return std::unexpected(eval_error{
        .kind = eval_error_kind::io_error,
        .message = "failed to open file",
        .file = path.string(),
        .line = 0,
        .column = 0,
    });
  }

  std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  // Set the import base path for relative imports
  if constexpr (runtime::has_io()) {
    if (auto* io = executor_->io(); io != nullptr) {
      io->set_import_base_path(std::filesystem::canonical(path));
    }
  }

  // Compile and execute
  auto wasm = compile_source(source, path.string());
  if (!wasm) {
    return std::unexpected(wasm.error());
  }

  auto result = executor_->execute(*wasm);
  if (!result.success) {
    return std::unexpected(eval_error{
        .kind = eval_error_kind::runtime_error,
        .message = result.error,
        .file = path.string(),
        .line = result.line,
        .column = result.column,
    });
  }

  return result.value;
}

auto evaluator::eval_file(const std::filesystem::path& path) -> eval_result<std::string> {
  auto value = eval_file_raw(path);
  if (!value) {
    return std::unexpected(value.error());
  }

  return executor_->format_value(*value);
}

auto evaluator::eval_source(std::string_view source, std::string_view path) -> compile::nix_value {
  // Compile
  auto wasm = compile_source(source, path);
  if (!wasm) {
    throw std::runtime_error("import: parse/compile error in " + std::string(path) + ": " +
                             wasm.error().message);
  }

  // Execute within the existing context if available (for imports during evaluation)
  // This ensures imported values are allocated in the same memory space.
  runtime::execution_result result;
  if (executor_->has_context()) {
    result = executor_->execute_within(*wasm);
  } else {
    result = executor_->execute(*wasm);
  }

  if (!result.success) {
    throw std::runtime_error("import: evaluation error in " + std::string(path) + ": " +
                             result.error);
  }

  return result.value;
}

} // namespace straylight::nix::compiler
