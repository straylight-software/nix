// straylight // nix-language // fuzz
//
// Fuzz test harness for the Nix compiler
//
// Build with: clang++ -g -fsanitize=fuzzer,address,undefined ...
// Run with: ./fuzz_compile -max_len=4096

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "straylight/language/ast/expression.h"
#include "straylight/language/ast/symbol_table.h"
#include "straylight/language/compile/compiler.h"
#include "straylight/language/parse/parser.h"

using namespace straylight::language;

// Fuzz target: parse and compile arbitrary input
// Goal: Ensure the compiler never crashes on valid ASTs
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Limit input size to avoid timeout
  if (size > 10000) {
    return 0;
  }

  // Convert to string_view
  std::string_view input(reinterpret_cast<const char*>(data), size);

  try {
    // Parse
    ast::symbol_table symbols;
    auto expr = parse::parse(input, symbols);

    // Compile - this is the main target
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);

    // Validate the output
    (void)module.validate();

  } catch (const std::exception&) {
    // Parse/compile errors are expected for random input
  } catch (...) {
    // Catch-all
  }

  return 0;
}
