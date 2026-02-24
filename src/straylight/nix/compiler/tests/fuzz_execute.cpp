// straylight // nix-language // fuzz
//
// Fuzz test harness for full Nix evaluation pipeline
//
// This is the most comprehensive fuzz target - it exercises:
// - Parsing
// - AST construction
// - Compilation to WASM
// - WASM validation
// - WASM execution
// - Runtime operations
//
// Build with: clang++ -g -fsanitize=fuzzer,address,undefined ...
// Run with: ./fuzz_execute -max_len=4096

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "straylight/nix/compiler/ast/expression.h"
#include "straylight/nix/compiler/ast/symbol_table.h"
#include "straylight/nix/compiler/compile/compiler.h"
#include "straylight/nix/compiler/parse/parser.h"
#include "straylight/nix/compiler/runtime/wasm_executor.h"

using namespace straylight::nix::compiler;

// Fuzz target: parse, compile, and execute arbitrary input
// Goal: Ensure the entire pipeline never crashes
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Limit input size to avoid timeout
  if (size > 5000) {
    return 0;
  }

  // Convert to string_view (may contain null bytes, that's fine)
  std::string_view input(reinterpret_cast<const char*>(data), size);

  try {
    // 1. Parse
    ast::symbol_table symbols;
    auto expr = parse::parse(input, symbols);

    // 2. Compile
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);

    // 3. Validate (catches some compiler bugs)
    if (!module.validate()) {
      return 0; // invalid WASM is not a crash
    }

    // 4. Execute
    runtime::wasm_executor executor;
    auto result = executor.execute(module.emit_binary());

    // 5. If successful, try to format the result (exercises more code paths)
    if (result.success) {
      (void)executor.format_value(result.value);
    }

  } catch (const std::exception&) {
    // Expected for random input - parse errors, type errors, etc.
  } catch (...) {
    // Catch-all for any other exceptions
  }

  return 0;
}
