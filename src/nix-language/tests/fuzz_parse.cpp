// straylight // nix-language // fuzz
//
// Fuzz test harness for the Nix parser
//
// Build with: clang++ -g -fsanitize=fuzzer,address,undefined ...
// Run with: ./fuzz_parse -max_len=4096

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "nix-language/ast/expression.hh"
#include "nix-language/ast/symbol_table.hh"
#include "nix-language/parse/parser.hh"

using namespace nix::language;

// Fuzz target: try to parse arbitrary input
// Goal: Ensure the parser never crashes, regardless of input
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Limit input size to avoid timeout
  if (size > 10000) {
    return 0;
  }

  // Convert to string_view
  std::string_view input(reinterpret_cast<const char*>(data), size);

  // Try to parse - should never crash
  try {
    ast::symbol_table symbols;
    auto expr = parse::parse(input, symbols);
    // If parsing succeeds, expr should be valid
    (void)expr;
  } catch (const std::exception&) {
    // Parse errors are expected for random input
    // The important thing is we don't crash
  } catch (...) {
    // Catch-all for any other exceptions
    // Still shouldn't crash
  }

  return 0;
}
