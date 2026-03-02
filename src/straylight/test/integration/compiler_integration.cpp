// straylight // nix-language // tests
//
// Integration tests: compile Nix source to WASM and verify output
//
// These tests verify the complete pipeline:
//   Nix source -> parse -> AST -> compile -> WASM -> validate
//
// For actual execution, the WASM binary can be run with wasmtime:
//   wasmtime --invoke __main output.wasm

#include <cstdint>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compiler/compile/compiler.h"
#include "straylight/nix/compiler/parse/parser.h"
#include "straylight/nix/compiler/runtime/memory_layout.h"

// =============================================================================
// Helper: compile Nix source to WASM
// =============================================================================

struct compilation_result {
  bool valid;
  std::string wat;                  // WebAssembly Text format
  std::vector<std::uint8_t> binary; // WebAssembly binary
  std::string error;
};

auto compile_nix(std::string_view source) -> compilation_result {
  try {
    straylight::nix::compiler::ast::symbol_table symbols;
    auto expr = straylight::nix::compiler::parse::parse(source, symbols);
    straylight::nix::compiler::compile::compiler comp(symbols);
    auto module = comp.compile(expr);

    if (!module.validate()) {
      return {false, "", {}, "WASM validation failed"};
    }

    return {true, module.emit_text(), module.emit_binary(), ""};
  } catch (const std::exception& e) {
    return {false, "", {}, e.what()};
  }
}

/// Write WASM binary to file (for manual testing with wasmtime)
auto write_wasm_file(const std::string& filename, const std::vector<std::uint8_t>& binary) -> bool {
  std::ofstream file(filename, std::ios::binary);
  if (!file) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(binary.data()),
             static_cast<std::streamsize>(binary.size()));
  return file.good();
}

// =============================================================================
// Basic expressions
// =============================================================================

TEST_CASE("integration: integer literal", "[integration]") {
  auto result = compile_nix("42");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("i64.const") != std::string::npos);
  // The packed value for int 42 is: (42 << 32) | 2
  // = 0x0000002A00000002
}

TEST_CASE("integration: arithmetic", "[integration]") {
  auto result = compile_nix("1 + 2");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("call $__add") != std::string::npos);
}

TEST_CASE("integration: nested arithmetic", "[integration]") {
  auto result = compile_nix("(1 + 2) * 3");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("call $__add") != std::string::npos);
  REQUIRE(result.wat.find("call $__mul") != std::string::npos);
}

TEST_CASE("integration: string literal", "[integration]") {
  auto result = compile_nix("\"hello\"");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("data") != std::string::npos);
  REQUIRE(result.wat.find("hello") != std::string::npos);
}

TEST_CASE("integration: string interpolation", "[integration]") {
  auto result = compile_nix(R"("hello ${"world"}")");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("__concatStrings") != std::string::npos);
}

// =============================================================================
// Let expressions
// =============================================================================

TEST_CASE("integration: simple let", "[integration]") {
  auto result = compile_nix("let x = 1; in x");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("local.set") != std::string::npos);
  REQUIRE(result.wat.find("local.get") != std::string::npos);
}

TEST_CASE("integration: let with arithmetic", "[integration]") {
  auto result = compile_nix("let x = 1; y = 2; in x + y");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("call $__add") != std::string::npos);
}

TEST_CASE("integration: nested let", "[integration]") {
  auto result = compile_nix("let x = 1; in let y = 2; in x + y");
  REQUIRE(result.valid);
}

// =============================================================================
// Functions (lambdas)
// =============================================================================

TEST_CASE("integration: simple lambda", "[integration]") {
  auto result = compile_nix("x: x");
  REQUIRE(result.valid);
  // Should create a lambda function
  REQUIRE(result.wat.find("__lambda_") != std::string::npos);
}

TEST_CASE("integration: lambda application", "[integration]") {
  auto result = compile_nix("(x: x + 1) 2");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("call $__apply") != std::string::npos);
}

TEST_CASE("integration: closure captures variable", "[integration]") {
  auto result = compile_nix("let x = 1; in y: x + y");
  REQUIRE(result.valid);
  // Should capture x
  REQUIRE(result.wat.find("__makeClosure") != std::string::npos);
}

// =============================================================================
// Lists
// =============================================================================

TEST_CASE("integration: empty list", "[integration]") {
  auto result = compile_nix("[]");
  REQUIRE(result.valid);
}

TEST_CASE("integration: simple list", "[integration]") {
  auto result = compile_nix("[1 2 3]");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("__makeList") != std::string::npos);
}

TEST_CASE("integration: list with expressions", "[integration]") {
  auto result = compile_nix("[1 (2 + 3) 4]");
  REQUIRE(result.valid);
  // List elements should be thunks for lazy evaluation
  REQUIRE(result.wat.find("__makeThunk") != std::string::npos);
}

// =============================================================================
// Attribute sets
// =============================================================================

TEST_CASE("integration: empty attrset", "[integration]") {
  auto result = compile_nix("{}");
  REQUIRE(result.valid);
}

TEST_CASE("integration: simple attrset", "[integration]") {
  auto result = compile_nix("{ x = 1; y = 2; }");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("__makeAttrs") != std::string::npos);
}

TEST_CASE("integration: nested attrset", "[integration]") {
  auto result = compile_nix("{ a = { b = 1; }; }");
  REQUIRE(result.valid);
}

TEST_CASE("integration: attrset selection", "[integration]") {
  auto result = compile_nix("{ x = 1; }.x");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("__select") != std::string::npos);
}

TEST_CASE("integration: multi-segment path merging", "[integration]") {
  auto result = compile_nix("{ a.b = 1; a.c = 2; }");
  REQUIRE(result.valid);
  // Should compile without throwing "duplicate key" error
}

// =============================================================================
// Conditionals
// =============================================================================

TEST_CASE("integration: if expression", "[integration]") {
  auto result = compile_nix("if true then 1 else 2");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("if") != std::string::npos);
}

TEST_CASE("integration: assert expression", "[integration]") {
  auto result = compile_nix("assert true; 42");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("__throw") != std::string::npos);
}

// =============================================================================
// Lazy evaluation (thunks)
// =============================================================================

TEST_CASE("integration: thunks in list", "[integration]") {
  // NOTE: In Nix, [1 + 2] is a parse error. List elements need parens for operators.
  auto result = compile_nix("[(1 + 2)]");
  REQUIRE(result.valid);
  // Non-trivial expressions should be wrapped in thunks
  REQUIRE(result.wat.find("__makeThunk") != std::string::npos);
}

TEST_CASE("integration: thunks in attrset", "[integration]") {
  auto result = compile_nix("{ x = 1 + 2; }");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("__makeThunk") != std::string::npos);
}

TEST_CASE("integration: force in condition", "[integration]") {
  auto result = compile_nix("if true then 1 else 2");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("__force") != std::string::npos);
}

// =============================================================================
// Error positions
// =============================================================================

TEST_CASE("integration: error positions in assert", "[integration]") {
  auto result = compile_nix("assert false; 1");
  REQUIRE(result.valid);
  // __throw should have position arguments
  auto wat = result.wat;
  // Find __throw call - it should have 3 arguments (msg, line, col)
  REQUIRE(wat.find("call $__throw") != std::string::npos);
}

TEST_CASE("integration: error positions in select", "[integration]") {
  auto result = compile_nix("{}.x");
  REQUIRE(result.valid);
  // __select should have position arguments
  auto wat = result.wat;
  REQUIRE(wat.find("call $__select") != std::string::npos);
}

// =============================================================================
// Memory layout validation
// =============================================================================

TEST_CASE("integration: memory layout constants", "[integration][memory]") {
  namespace mem = straylight::nix::compiler::memory_layout;

  // Verify our memory layout makes sense
  REQUIRE(mem::DATA_SEGMENT_LIMIT <= mem::HEAP_BASE);
  REQUIRE(mem::HEAP_BASE < mem::DEFAULT_MEMORY_SIZE);
  REQUIRE(mem::ALIGNMENT == 8);

  // Verify structure sizes are properly aligned
  REQUIRE(mem::THUNK_SIZE == 20);         // func_index(4) + env_ptr(4) + state(4) + cached(8)
  REQUIRE(mem::CLOSURE_HEADER_SIZE == 8); // func_index(4) + capture_count(4)
  REQUIRE(mem::ATTRSET_ENTRY_SIZE == 12); // key_offset(4) + value(8)
}

// =============================================================================
// Complex expressions
// =============================================================================

TEST_CASE("integration: recursive function", "[integration]") {
  auto result = compile_nix(R"(
    let
      fac = n: if n == 0 then 1 else n * fac (n - 1);
    in fac 5
  )");
  REQUIRE(result.valid);
}

TEST_CASE("integration: attrset pattern", "[integration]") {
  auto result = compile_nix("{ x, y }: x + y");
  REQUIRE(result.valid);
}

TEST_CASE("integration: attrset pattern with default", "[integration]") {
  auto result = compile_nix("{ x, y ? 0 }: x + y");
  REQUIRE(result.valid);
}

TEST_CASE("integration: with expression", "[integration]") {
  auto result = compile_nix("with { x = 1; }; x");
  REQUIRE(result.valid);
  REQUIRE(result.wat.find("__hasAttr") != std::string::npos);
  REQUIRE(result.wat.find("__select") != std::string::npos);
}

TEST_CASE("integration: inherit", "[integration]") {
  auto result = compile_nix("let x = 1; in { inherit x; }");
  REQUIRE(result.valid);
}

TEST_CASE("integration: inherit from", "[integration]") {
  auto result = compile_nix("{ inherit ({ x = 1; }) x; }");
  REQUIRE(result.valid);
}

// =============================================================================
// Binary output (for manual testing)
// =============================================================================

TEST_CASE("integration: can emit valid WASM binary", "[integration]") {
  auto result = compile_nix("1 + 2");
  REQUIRE(result.valid);
  REQUIRE(result.binary.size() > 0);

  // WASM magic number: 0x00 0x61 0x73 0x6D (\0asm)
  REQUIRE(result.binary.size() >= 4);
  REQUIRE(result.binary[0] == 0x00);
  REQUIRE(result.binary[1] == 0x61);
  REQUIRE(result.binary[2] == 0x73);
  REQUIRE(result.binary[3] == 0x6D);
}

// Uncomment to write a test WASM file:
// TEST_CASE("integration: write WASM file", "[integration][.manual]") {
//   auto result = compile_nix("let f = x: x * 2; in f 21");
//   REQUIRE(result.valid);
//   REQUIRE(write_wasm_file("/tmp/test.wasm", result.binary));
//   INFO("WASM written to /tmp/test.wasm");
//   INFO("Run with: wasmtime --invoke __main /tmp/test.wasm");
// }
