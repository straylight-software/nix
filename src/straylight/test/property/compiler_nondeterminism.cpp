// straylight // nix // compiler // tests
//
// Tests for AST walker nondeterminism bugs
//
// These tests prove that certain operations produce nondeterministic results
// due to iteration over unordered containers (unordered_map, unordered_set).
//
// KNOWN ISSUES:
// 1. compiler.h:143 - free variable set converted from unordered_set to vector
// 2. compiler.h:1478 - nested_groups unordered_map iteration for attrset construction
// 3. topo_sort.h:144 - in_degree unordered_map iteration for ready queue
//
// These tests are designed to FAIL when the bug exists, proving the issue.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compiler/compile/compiler.h"
#include "straylight/nix/compiler/compile/wasm_types.h"
#include "straylight/nix/compiler/parse/parser.h"
#include "straylight/nix/compiler/runtime/wasm_executor.h"

// =============================================================================
// Helper: compile and execute Nix source, returning formatted result
// =============================================================================

struct eval_result_t {
  bool success;
  straylight::nix::compiler::runtime::nix_value value;
  std::string error;
  std::string formatted;
};

auto eval_nix(std::string_view source) -> eval_result_t {
  try {
    straylight::nix::compiler::ast::symbol_table symbols;
    auto expr = straylight::nix::compiler::parse::parse(source, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto module = comp.compile(expr);

    if (!module.validate()) {
      return {false, 0, "WASM validation failed", ""};
    }

    straylight::nix::compiler::runtime::wasm_executor executor;
    auto result = executor.execute(module.emit_binary());

    if (!result.success) {
      return {false, 0, result.error, ""};
    }

    return {true, result.value, "", executor.format_value(result.value)};

  } catch (const std::exception& e) {
    return {false, 0, e.what(), ""};
  }
}

// =============================================================================
// Helper: compile to WASM binary (for comparing codegen)
// =============================================================================

auto compile_to_wasm(std::string_view source) -> std::vector<std::uint8_t> {
  straylight::nix::compiler::ast::symbol_table symbols;
  auto expr = straylight::nix::compiler::parse::parse(source, symbols);
  straylight::nix::compiler::compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  return module.emit_binary();
}

// =============================================================================
// TEST: Attrset construction ordering (compiler.h:1478)
//
// The compiler groups nested bindings using an unordered_map, then iterates
// to build the attrset. This can produce different attribute orderings.
//
// For attrsets with the same keys, Nix semantics require the same values
// regardless of internal ordering. But the WASM binary/bytecode will differ.
// =============================================================================

TEST_CASE("nondeterminism: attrset construction produces deterministic WASM",
          "[nondeterminism][attrset]") {
  // Attrset with many keys to increase collision probability
  constexpr auto source = R"nix(
    {
      alpha = 1;
      beta = 2;
      gamma = 3;
      delta = 4;
      epsilon = 5;
      zeta = 6;
      eta = 7;
      theta = 8;
      iota = 9;
      kappa = 10;
    }
  )nix";

  // Compile multiple times and verify WASM binary is identical
  constexpr int iterations = 10;
  std::set<std::vector<std::uint8_t>> unique_binaries;

  for (int i = 0; i < iterations; ++i) {
    auto binary = compile_to_wasm(source);
    unique_binaries.insert(binary);
  }

  INFO("Found " << unique_binaries.size() << " unique WASM binaries across " << iterations
                << " compilations");
  INFO("Expected 1 unique binary for deterministic compilation");

  // This test PASSES if compilation is deterministic
  // If it FAILS, the nondeterminism bug exists
  REQUIRE(unique_binaries.size() == 1);
}

TEST_CASE("nondeterminism: nested attrset construction is deterministic",
          "[nondeterminism][attrset]") {
  // Nested attrset merging uses unordered_map for grouping
  constexpr auto source = R"nix(
    {
      a.x = 1;
      a.y = 2;
      b.x = 3;
      b.y = 4;
      c.x = 5;
      c.y = 6;
      d.x = 7;
      d.y = 8;
    }
  )nix";

  constexpr int iterations = 10;
  std::set<std::vector<std::uint8_t>> unique_binaries;

  for (int i = 0; i < iterations; ++i) {
    auto binary = compile_to_wasm(source);
    unique_binaries.insert(binary);
  }

  INFO("Found " << unique_binaries.size() << " unique WASM binaries across " << iterations
                << " compilations");
  REQUIRE(unique_binaries.size() == 1);
}

// =============================================================================
// TEST: Free variable analysis ordering (compiler.h:143)
//
// The free variable analyzer uses unordered_set<symbol>, then converts to
// vector with (begin, end). This produces nondeterministic capture ordering.
//
// For closures, this affects the layout of captured variables in the thunk.
// =============================================================================

TEST_CASE("nondeterminism: lambda free variable capture is deterministic",
          "[nondeterminism][lambda]") {
  // Lambda capturing multiple free variables
  // The order they're captured should be deterministic
  constexpr auto source = R"nix(
    let
      a = 1;
      b = 2;
      c = 3;
      d = 4;
      e = 5;
      f = 6;
      g = 7;
      h = 8;
    in
      x: a + b + c + d + e + f + g + h + x
  )nix";

  constexpr int iterations = 10;
  std::set<std::vector<std::uint8_t>> unique_binaries;

  for (int i = 0; i < iterations; ++i) {
    auto binary = compile_to_wasm(source);
    unique_binaries.insert(binary);
  }

  INFO("Found " << unique_binaries.size() << " unique WASM binaries across " << iterations
                << " compilations");
  REQUIRE(unique_binaries.size() == 1);
}

TEST_CASE("nondeterminism: nested lambda captures are deterministic", "[nondeterminism][lambda]") {
  // Nested lambdas with overlapping captures
  constexpr auto source = R"nix(
    let
      p = 1;
      q = 2;
      r = 3;
      s = 4;
    in
      x: y: p + q + r + s + x + y
  )nix";

  constexpr int iterations = 10;
  std::set<std::vector<std::uint8_t>> unique_binaries;

  for (int i = 0; i < iterations; ++i) {
    auto binary = compile_to_wasm(source);
    unique_binaries.insert(binary);
  }

  INFO("Found " << unique_binaries.size() << " unique WASM binaries across " << iterations
                << " compilations");
  REQUIRE(unique_binaries.size() == 1);
}

// =============================================================================
// TEST: builtins.attrNames ordering
//
// While attrNames should return sorted names (per Nix semantics),
// the internal attrset iteration should not affect the sorted output.
// This test verifies the OUTPUT is deterministic, not just the codegen.
// =============================================================================

TEST_CASE("nondeterminism: builtins.attrNames returns sorted keys", "[nondeterminism][builtins]") {
  // Test that the first element is "alpha" (the sorted-first key)
  // This proves keys are in sorted order
  constexpr auto source = R"nix(
    builtins.head (builtins.attrNames {
      zebra = 1;
      alpha = 2;
      omega = 3;
      beta = 4;
      gamma = 5;
    })
  )nix";

  // Run evaluation multiple times and check for identical results
  constexpr int iterations = 10;
  std::set<std::string> unique_results;

  for (int i = 0; i < iterations; ++i) {
    auto result = eval_nix(source);
    INFO("Error: " << result.error);
    REQUIRE(result.success);
    unique_results.insert(result.formatted);
  }

  INFO("Found " << unique_results.size() << " unique results across " << iterations
                << " evaluations");

  // Should always produce the same sorted list
  REQUIRE(unique_results.size() == 1);

  // Verify it's actually sorted (first key should be "alpha")
  auto result = eval_nix(source);
  REQUIRE(result.formatted == "\"alpha\"");
}

// =============================================================================
// TEST: builtins.attrValues ordering
//
// attrValues should return values in the same order as attrNames (sorted).
// =============================================================================

TEST_CASE("nondeterminism: builtins.attrValues returns deterministic order",
          "[nondeterminism][builtins]") {
  // Test that the first value is 1 (corresponding to key "a", the sorted-first key)
  constexpr auto source = R"nix(
    builtins.head (builtins.attrValues {
      c = 3;
      a = 1;
      b = 2;
      d = 4;
      e = 5;
    })
  )nix";

  constexpr int iterations = 10;
  std::set<std::string> unique_results;

  for (int i = 0; i < iterations; ++i) {
    auto result = eval_nix(source);
    INFO("Error: " << result.error);
    REQUIRE(result.success);
    unique_results.insert(result.formatted);
  }

  INFO("Found " << unique_results.size() << " unique results across " << iterations
                << " evaluations");
  REQUIRE(unique_results.size() == 1);

  // Verify first value is 1 (value of "a", the sorted-first key)
  auto result = eval_nix(source);
  REQUIRE(result.formatted == "1");
}

// =============================================================================
// TEST: let-binding ordering
//
// let bindings are topologically sorted. If the topo sort is nondeterministic,
// bindings may be evaluated in different orders (though results should be same).
// =============================================================================

TEST_CASE("nondeterminism: let binding evaluation order is deterministic",
          "[nondeterminism][let]") {
  // Independent bindings - topo sort can order them arbitrarily
  constexpr auto source = R"nix(
    let
      a = 1;
      b = 2;
      c = 3;
      d = 4;
      e = 5;
    in
      { inherit a b c d e; }
  )nix";

  constexpr int iterations = 10;
  std::set<std::vector<std::uint8_t>> unique_binaries;

  for (int i = 0; i < iterations; ++i) {
    auto binary = compile_to_wasm(source);
    unique_binaries.insert(binary);
  }

  INFO("Found " << unique_binaries.size() << " unique WASM binaries across " << iterations
                << " compilations");
  REQUIRE(unique_binaries.size() == 1);
}

TEST_CASE("nondeterminism: rec attrset with dependencies is deterministic",
          "[nondeterminism][rec]") {
  // Recursive attrset with dependencies
  // Topo sort must order correctly AND deterministically
  constexpr auto source = R"nix(
    rec {
      a = 1;
      b = a + 1;
      c = b + 1;
      d = 10;
      e = d + c;
    }
  )nix";

  constexpr int iterations = 10;
  std::set<std::vector<std::uint8_t>> unique_binaries;

  for (int i = 0; i < iterations; ++i) {
    auto binary = compile_to_wasm(source);
    unique_binaries.insert(binary);
  }

  INFO("Found " << unique_binaries.size() << " unique WASM binaries across " << iterations
                << " compilations");
  REQUIRE(unique_binaries.size() == 1);
}

// =============================================================================
// TEST: builtins.groupBy is now deterministic
//
// groupBy was fixed to use std::map instead of std::unordered_map.
// This test verifies the fix is working.
// =============================================================================

TEST_CASE("nondeterminism: builtins.groupBy returns deterministic order",
          "[nondeterminism][builtins][groupby]") {
  constexpr auto source = R"nix(
    builtins.groupBy (x: x.type) [
      { type = "fruit"; name = "apple"; }
      { type = "vegetable"; name = "carrot"; }
      { type = "fruit"; name = "banana"; }
      { type = "vegetable"; name = "broccoli"; }
      { type = "fruit"; name = "cherry"; }
    ]
  )nix";

  constexpr int iterations = 10;
  std::set<std::string> unique_results;

  for (int i = 0; i < iterations; ++i) {
    auto result = eval_nix(source);
    INFO("Iteration " << i << ": " << result.formatted);
    INFO("Error: " << result.error);
    REQUIRE(result.success);
    unique_results.insert(result.formatted);
  }

  INFO("Found " << unique_results.size() << " unique results across " << iterations
                << " evaluations");
  REQUIRE(unique_results.size() == 1);
}

// =============================================================================
// TEST: Complex expression combining multiple potential nondeterminism sources
// =============================================================================

TEST_CASE("nondeterminism: complex expression is fully deterministic",
          "[nondeterminism][complex]") {
  constexpr auto source = R"nix(
    let
      data = {
        users = [
          { name = "alice"; role = "admin"; }
          { name = "bob"; role = "user"; }
          { name = "carol"; role = "admin"; }
          { name = "dave"; role = "user"; }
        ];
        config = {
          a.x = 1;
          a.y = 2;
          b.x = 3;
          b.y = 4;
        };
      };
      process = x: y: x.name + "-" + y;
    in
      {
        names = builtins.map (u: u.name) data.users;
        grouped = builtins.groupBy (u: u.role) data.users;
        keys = builtins.attrNames data.config;
      }
  )nix";

  constexpr int iterations = 10;
  std::set<std::vector<std::uint8_t>> unique_binaries;
  std::set<std::string> unique_results;

  for (int i = 0; i < iterations; ++i) {
    auto binary = compile_to_wasm(source);
    unique_binaries.insert(binary);

    auto result = eval_nix(source);
    if (result.success) {
      unique_results.insert(result.formatted);
    }
  }

  INFO("Found " << unique_binaries.size() << " unique WASM binaries across " << iterations
                << " compilations");
  INFO("Found " << unique_results.size() << " unique results across " << iterations
                << " evaluations");

  REQUIRE(unique_binaries.size() == 1);
  REQUIRE(unique_results.size() == 1);
}
