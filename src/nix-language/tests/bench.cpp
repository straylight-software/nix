// straylight // nix-language // benchmarks
//
// Microbenchmarks using nanobench
//
// Run with: buck2 run //src/nix-language/tests:bench

#define ANKERL_NANOBENCH_IMPLEMENT
#include <cstdint>
#include <string>
#include <vector>

#include <nanobench.h>

#include "nix-language/ast/expression.hh"
#include "nix-language/ast/symbol_table.hh"
#include "nix-language/compile/compiler.hh"
#include "nix-language/compile/wasm_types.hh"
#include "nix-language/parse/parser.hh"
#include "nix-language/runtime/memory_layout.hh"
#include "nix-language/runtime/runtime.hh"
#include "nix-language/runtime/wasm_executor.hh"

using namespace nix::language;
using namespace nix::language::runtime;
using namespace nix::language::compile;
namespace mem = nix::language::memory_layout;

// =============================================================================
// Parsing benchmarks
// =============================================================================

void bench_parse_integer() {
  ankerl::nanobench::Bench().run("parse: integer literal", [&] {
    ast::symbol_table symbols;
    auto expr = parse::parse("42", symbols);
    ankerl::nanobench::doNotOptimizeAway(expr);
  });
}

void bench_parse_arithmetic() {
  ankerl::nanobench::Bench().run("parse: simple arithmetic", [&] {
    ast::symbol_table symbols;
    auto expr = parse::parse("1 + 2 * 3 - 4 / 2", symbols);
    ankerl::nanobench::doNotOptimizeAway(expr);
  });
}

void bench_parse_let() {
  ankerl::nanobench::Bench().run("parse: let expression", [&] {
    ast::symbol_table symbols;
    auto expr = parse::parse("let x = 1; y = 2; z = 3; in x + y + z", symbols);
    ankerl::nanobench::doNotOptimizeAway(expr);
  });
}

void bench_parse_lambda() {
  ankerl::nanobench::Bench().run("parse: lambda", [&] {
    ast::symbol_table symbols;
    auto expr = parse::parse("x: y: z: x + y * z", symbols);
    ankerl::nanobench::doNotOptimizeAway(expr);
  });
}

void bench_parse_attrset() {
  ankerl::nanobench::Bench().run("parse: attrset", [&] {
    ast::symbol_table symbols;
    auto expr = parse::parse("{ a = 1; b = 2; c = 3; d = { e = 4; f = 5; }; }", symbols);
    ankerl::nanobench::doNotOptimizeAway(expr);
  });
}

void bench_parse_list() {
  ankerl::nanobench::Bench().run("parse: list", [&] {
    ast::symbol_table symbols;
    auto expr = parse::parse("[ 1 2 3 4 5 6 7 8 9 10 ]", symbols);
    ankerl::nanobench::doNotOptimizeAway(expr);
  });
}

void bench_parse_complex() {
  ankerl::nanobench::Bench().run("parse: complex expression", [&] {
    ast::symbol_table symbols;
    auto expr = parse::parse(
        R"(
        let
          id = x: x;
          const = x: y: x;
          flip = f: x: y: f y x;
          compose = f: g: x: f (g x);
        in
          compose (x: x + 1) (x: x * 2) 10
        )",
        symbols);
    ankerl::nanobench::doNotOptimizeAway(expr);
  });
}

// =============================================================================
// Compilation benchmarks
// =============================================================================

void bench_compile_integer() {
  ast::symbol_table symbols;
  auto expr = parse::parse("42", symbols);

  ankerl::nanobench::Bench().run("compile: integer literal", [&] {
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    ankerl::nanobench::doNotOptimizeAway(module);
  });
}

void bench_compile_arithmetic() {
  ast::symbol_table symbols;
  auto expr = parse::parse("1 + 2 * 3 - 4 / 2", symbols);

  ankerl::nanobench::Bench().run("compile: arithmetic", [&] {
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    ankerl::nanobench::doNotOptimizeAway(module);
  });
}

void bench_compile_lambda() {
  ast::symbol_table symbols;
  auto expr = parse::parse("let f = x: x + 1; in f 5", symbols);

  ankerl::nanobench::Bench().run("compile: lambda", [&] {
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    ankerl::nanobench::doNotOptimizeAway(module);
  });
}

void bench_compile_closure() {
  ast::symbol_table symbols;
  auto expr = parse::parse("let a = 1; b = 2; in (x: a + b + x) 3", symbols);

  ankerl::nanobench::Bench().run("compile: closure", [&] {
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    ankerl::nanobench::doNotOptimizeAway(module);
  });
}

void bench_compile_attrset() {
  ast::symbol_table symbols;
  auto expr = parse::parse("{ a = 1; b = 2; c = 3; d = 4; e = 5; }", symbols);

  ankerl::nanobench::Bench().run("compile: attrset", [&] {
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    ankerl::nanobench::doNotOptimizeAway(module);
  });
}

// =============================================================================
// Execution benchmarks
// =============================================================================

void bench_exec_integer() {
  ast::symbol_table symbols;
  auto expr = parse::parse("42", symbols);
  compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  auto binary = module.emit_binary();

  ankerl::nanobench::Bench().run("exec: integer literal", [&] {
    wasm_executor executor;
    auto result = executor.execute(binary);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_exec_arithmetic() {
  ast::symbol_table symbols;
  auto expr = parse::parse("1 + 2 * 3 - 4 / 2", symbols);
  compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  auto binary = module.emit_binary();

  ankerl::nanobench::Bench().run("exec: arithmetic", [&] {
    wasm_executor executor;
    auto result = executor.execute(binary);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_exec_let() {
  ast::symbol_table symbols;
  auto expr = parse::parse("let x = 1; y = 2; z = 3; in x + y + z", symbols);
  compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  auto binary = module.emit_binary();

  ankerl::nanobench::Bench().run("exec: let expression", [&] {
    wasm_executor executor;
    auto result = executor.execute(binary);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_exec_lambda() {
  ast::symbol_table symbols;
  auto expr = parse::parse("(x: x + 1) 5", symbols);
  compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  auto binary = module.emit_binary();

  ankerl::nanobench::Bench().run("exec: lambda application", [&] {
    wasm_executor executor;
    auto result = executor.execute(binary);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_exec_closure() {
  ast::symbol_table symbols;
  auto expr = parse::parse("let a = 1; b = 2; in (x: a + b + x) 3", symbols);
  compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  auto binary = module.emit_binary();

  ankerl::nanobench::Bench().run("exec: closure", [&] {
    wasm_executor executor;
    auto result = executor.execute(binary);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_exec_attrset() {
  ast::symbol_table symbols;
  auto expr = parse::parse("{ a = 1; b = 2; c = 3; }.b", symbols);
  compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  auto binary = module.emit_binary();

  ankerl::nanobench::Bench().run("exec: attrset select", [&] {
    wasm_executor executor;
    auto result = executor.execute(binary);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_exec_conditional() {
  ast::symbol_table symbols;
  auto expr = parse::parse("if 1 < 2 then 42 else 0", symbols);
  compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  auto binary = module.emit_binary();

  ankerl::nanobench::Bench().run("exec: conditional", [&] {
    wasm_executor executor;
    auto result = executor.execute(binary);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_exec_recursive() {
  ast::symbol_table symbols;
  auto expr = parse::parse("rec { x = 1; y = x + 1; z = y + 1; }.z", symbols);
  compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  auto binary = module.emit_binary();

  ankerl::nanobench::Bench().run("exec: recursive attrset", [&] {
    wasm_executor executor;
    auto result = executor.execute(binary);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

// =============================================================================
// Runtime benchmarks (no WASM overhead)
// =============================================================================

void bench_runtime_add() {
  runtime_context ctx;
  auto a = make_int(123);
  auto b = make_int(456);

  ankerl::nanobench::Bench().run("runtime: rt_add", [&] {
    auto result = rt_add(ctx, a, b, 0, 0);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_runtime_eq() {
  runtime_context ctx;
  auto a = make_int(42);
  auto b = make_int(42);

  ankerl::nanobench::Bench().run("runtime: rt_eq", [&] {
    auto result = rt_eq(ctx, a, b);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_runtime_not() {
  runtime_context ctx;
  auto v = constants::bool_true;

  ankerl::nanobench::Bench().run("runtime: rt_not", [&] {
    auto result = rt_not(ctx, v);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_heap_allocate() {
  ankerl::nanobench::Bench().run("runtime: heap allocate", [&] {
    heap_allocator heap(mem::HEAP_BASE, mem::DEFAULT_MEMORY_SIZE);
    for (int i = 0; i < 100; ++i) {
      auto ptr = heap.allocate(64);
      ankerl::nanobench::doNotOptimizeAway(ptr);
    }
  });
}

void bench_memory_layout_align() {
  ankerl::nanobench::Bench().run("runtime: align_up", [&] {
    for (std::uint32_t i = 0; i < 1000; ++i) {
      auto aligned = mem::align_up(i);
      ankerl::nanobench::doNotOptimizeAway(aligned);
    }
  });
}

// =============================================================================
// Symbol table benchmarks
// =============================================================================

void bench_symbol_intern() {
  ankerl::nanobench::Bench().run("ast: symbol intern (new)", [&] {
    ast::symbol_table symbols;
    for (int i = 0; i < 100; ++i) {
      auto sym = symbols.intern("variable" + std::to_string(i));
      ankerl::nanobench::doNotOptimizeAway(sym);
    }
  });
}

void bench_symbol_intern_existing() {
  ast::symbol_table symbols;
  // Pre-intern the symbols
  for (int i = 0; i < 100; ++i) {
    symbols.intern("variable" + std::to_string(i));
  }

  ankerl::nanobench::Bench().run("ast: symbol intern (existing)", [&] {
    for (int i = 0; i < 100; ++i) {
      auto sym = symbols.intern("variable" + std::to_string(i));
      ankerl::nanobench::doNotOptimizeAway(sym);
    }
  });
}

void bench_symbol_lookup() {
  ast::symbol_table symbols;
  std::vector<ast::symbol> syms;
  for (int i = 0; i < 100; ++i) {
    syms.push_back(symbols.intern("variable" + std::to_string(i)));
  }

  ankerl::nanobench::Bench().run("ast: symbol lookup", [&] {
    for (auto sym : syms) {
      auto name = symbols.lookup(sym);
      ankerl::nanobench::doNotOptimizeAway(name);
    }
  });
}

// =============================================================================
// End-to-end pipeline benchmarks
// =============================================================================

void bench_e2e_simple() {
  ankerl::nanobench::Bench().run("e2e: simple (42)", [&] {
    ast::symbol_table symbols;
    auto expr = parse::parse("42", symbols);
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    wasm_executor executor;
    auto result = executor.execute(module.emit_binary());
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_e2e_medium() {
  ankerl::nanobench::Bench().run("e2e: medium (let + lambda)", [&] {
    ast::symbol_table symbols;
    auto expr = parse::parse("let f = x: x + 1; in f 41", symbols);
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    wasm_executor executor;
    auto result = executor.execute(module.emit_binary());
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

void bench_e2e_complex() {
  ankerl::nanobench::Bench().run("e2e: complex (rec attrset)", [&] {
    ast::symbol_table symbols;
    auto expr =
        parse::parse("rec { a = 1; b = a + 1; c = b + 1; d = c + 1; e = d + 1; }.e", symbols);
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    wasm_executor executor;
    auto result = executor.execute(module.emit_binary());
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

// =============================================================================
// Main
// =============================================================================

int main() {
  std::cout << "=== nix-language benchmarks ===\n\n";

  std::cout << "--- Parsing ---\n";
  bench_parse_integer();
  bench_parse_arithmetic();
  bench_parse_let();
  bench_parse_lambda();
  bench_parse_attrset();
  bench_parse_list();
  bench_parse_complex();

  std::cout << "\n--- Compilation ---\n";
  bench_compile_integer();
  bench_compile_arithmetic();
  bench_compile_lambda();
  bench_compile_closure();
  bench_compile_attrset();

  std::cout << "\n--- Execution ---\n";
  bench_exec_integer();
  bench_exec_arithmetic();
  bench_exec_let();
  bench_exec_lambda();
  bench_exec_closure();
  bench_exec_attrset();
  bench_exec_conditional();
  bench_exec_recursive();

  std::cout << "\n--- Runtime (no WASM) ---\n";
  bench_runtime_add();
  bench_runtime_eq();
  bench_runtime_not();
  bench_heap_allocate();
  bench_memory_layout_align();

  std::cout << "\n--- Symbol Table ---\n";
  bench_symbol_intern();
  bench_symbol_intern_existing();
  bench_symbol_lookup();

  std::cout << "\n--- End-to-End Pipeline ---\n";
  bench_e2e_simple();
  bench_e2e_medium();
  bench_e2e_complex();

  return 0;
}
