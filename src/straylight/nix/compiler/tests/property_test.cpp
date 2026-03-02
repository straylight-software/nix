// straylight // nix-language // tests
//
// Property-based tests using RapidCheck
//
// These tests generate random inputs and verify invariants hold for ALL inputs,
// not just hand-picked examples. This catches edge cases humans miss.

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compiler/ast/expression.h"
#include "straylight/nix/compiler/ast/symbol_table.h"
#include "straylight/nix/compiler/compile/compiler.h"
#include "straylight/nix/compiler/compile/wasm_types.h"
#include "straylight/nix/compiler/parse/parser.h"
#include "straylight/nix/compiler/runtime/memory_layout.h"
#include "straylight/nix/compiler/runtime/runtime.h"
#include "straylight/nix/compiler/runtime/wasm_executor.h"


// =============================================================================
// Generators for Nix types
// =============================================================================

// Generate a valid Nix identifier
auto gen_identifier() -> rc::Gen<std::string> {
  return rc::gen::apply(
      [](char first, const std::string& rest) {
        std::string id(1, first);
        for (char c : rest) {
          if (id.size() < 20) {
            id += c; // limit length
          }
        }
        return id;
      },
      rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('A', 'Z'), rc::gen::just('_')),
      rc::gen::container<std::string>(
          rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('A', 'Z'),
                         rc::gen::inRange('0', '9'), rc::gen::just('_'))));
}

// Generate integers in safe range (avoid overflow)
auto gen_safe_int() -> rc::Gen<std::int32_t> {
  return rc::gen::inRange<std::int32_t>(-1000000, 1000000);
}

// Generate simple Nix integer expressions
auto gen_int_expr() -> rc::Gen<std::string> {
  return rc::gen::map(gen_safe_int(), [](std::int32_t n) { return std::to_string(n); });
}

// =============================================================================
// Value packing/unpacking properties
// =============================================================================

TEST_CASE("property: value tag round-trip", "[property][wasm_types]") {
  rc::check("tag survives round-trip", [](std::uint8_t raw_tag) {
    if (raw_tag > 10) {
      return; // only valid tags
    }
    auto tag = static_cast<straylight::nix::compiler::compile::value_tag>(raw_tag);
    auto payload = *rc::gen::inRange<std::uint32_t>(0, 0xFFFFFFFF);
    auto packed = straylight::nix::compiler::runtime::make_value(tag, payload);
    RC_ASSERT(straylight::nix::compiler::runtime::get_tag(packed) == tag);
    RC_ASSERT(straylight::nix::compiler::runtime::get_payload(packed) == payload);
  });
}

TEST_CASE("property: integer value encoding", "[property][wasm_types]") {
  rc::check("integers encode correctly", []() {
    auto n = *rc::gen::inRange<std::int32_t>(std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
    auto packed = straylight::nix::compiler::runtime::make_int(n);
    RC_ASSERT(straylight::nix::compiler::runtime::is_int(packed));
    RC_ASSERT(!straylight::nix::compiler::runtime::is_bool(packed));
    RC_ASSERT(!straylight::nix::compiler::runtime::is_string(packed));
    RC_ASSERT(straylight::nix::compiler::runtime::get_int_value(packed) == n);
  });
}

TEST_CASE("property: boolean value encoding", "[property][wasm_types]") {
  rc::check("booleans encode correctly", [](bool b) {
    auto packed = straylight::nix::compiler::runtime::make_bool(b);
    RC_ASSERT(straylight::nix::compiler::runtime::is_bool(packed));
    RC_ASSERT(!straylight::nix::compiler::runtime::is_int(packed));
    RC_ASSERT(straylight::nix::compiler::runtime::get_bool_value(packed) == b);
  });
}

// =============================================================================
// Memory layout properties
// =============================================================================

TEST_CASE("property: alignment always produces aligned values", "[property][memory]") {
  namespace mem = straylight::nix::compiler::memory_layout;
  rc::check("align_up produces aligned values", []() {
    namespace mem = straylight::nix::compiler::memory_layout;
    auto size = *rc::gen::inRange<std::uint32_t>(0, 0x100000);
    auto aligned = mem::align_up(size);
    RC_ASSERT(aligned % mem::ALIGNMENT == 0);
    RC_ASSERT(aligned >= size);
    RC_ASSERT(aligned - size < mem::ALIGNMENT);
  });
}

TEST_CASE("property: list size calculation", "[property][memory]") {
  rc::check("list size is header + elements", []() {
    namespace mem = straylight::nix::compiler::memory_layout;
    auto count = *rc::gen::inRange<std::uint32_t>(0, 10000);
    auto size = mem::list_size(count);
    RC_ASSERT(size == mem::LIST_HEADER_SIZE + count * mem::VALUE_SIZE);
    RC_ASSERT(size >= mem::LIST_HEADER_SIZE);
  });
}

TEST_CASE("property: attrset size calculation", "[property][memory]") {
  rc::check("attrset size is header + entries", []() {
    namespace mem = straylight::nix::compiler::memory_layout;
    auto count = *rc::gen::inRange<std::uint32_t>(0, 10000);
    auto size = mem::attrset_size(count);
    RC_ASSERT(size == mem::ATTRSET_HEADER_SIZE + count * mem::ATTRSET_ENTRY_SIZE);
  });
}

TEST_CASE("property: closure size calculation", "[property][memory]") {
  rc::check("closure size is header + captures", []() {
    namespace mem = straylight::nix::compiler::memory_layout;
    auto count = *rc::gen::inRange<std::uint32_t>(0, 10000);
    auto size = mem::closure_size(count);
    RC_ASSERT(size == mem::CLOSURE_HEADER_SIZE + count * mem::VALUE_SIZE);
  });
}

// =============================================================================
// Heap allocator properties
// =============================================================================

TEST_CASE("property: heap allocator never returns overlapping regions", "[property][runtime]") {
  rc::check("allocations don't overlap", []() {
    namespace mem = straylight::nix::compiler::memory_layout;
    straylight::nix::compiler::runtime::heap_allocator heap(mem::HEAP_BASE,
                                                            mem::DEFAULT_MEMORY_SIZE);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> allocations;
    auto num_allocs = *rc::gen::inRange(1, 100);

    for (int idx = 0; idx < num_allocs; ++idx) {
      auto size = *rc::gen::inRange<std::uint32_t>(1, 1000);
      try {
        auto ptr = heap.allocate(size);
        auto aligned_size = mem::align_up(size);
        allocations.emplace_back(ptr, ptr + aligned_size);
      } catch (const straylight::nix::compiler::runtime::oom_error&) {
        break; // ran out of memory, that's fine
      }
    }

    // verify no overlaps
    for (size_t idx = 0; idx < allocations.size(); ++idx) {
      for (size_t jdx = idx + 1; jdx < allocations.size(); ++jdx) {
        auto& [a_start, a_end] = allocations[idx];
        auto& [b_start, b_end] = allocations[jdx];
        RC_ASSERT(a_end <= b_start || b_end <= a_start);
      }
    }
  });
}

TEST_CASE("property: heap allocator returns aligned pointers", "[property][runtime]") {
  rc::check("allocations are aligned", []() {
    namespace mem = straylight::nix::compiler::memory_layout;
    straylight::nix::compiler::runtime::heap_allocator heap(mem::HEAP_BASE,
                                                            mem::DEFAULT_MEMORY_SIZE);

    auto num_allocs = *rc::gen::inRange(1, 50);
    for (int idx = 0; idx < num_allocs; ++idx) {
      auto size = *rc::gen::inRange<std::uint32_t>(1, 500);
      try {
        auto ptr = heap.allocate(size);
        RC_ASSERT(ptr % mem::ALIGNMENT == 0);
      } catch (const straylight::nix::compiler::runtime::oom_error&) {
        break;
      }
    }
  });
}

// =============================================================================
// Arithmetic properties
// =============================================================================

TEST_CASE("property: integer addition is commutative", "[property][execution]") {
  rc::check("a + b == b + a", []() {
    auto a = *gen_safe_int();
    auto b = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols1, symbols2;
    auto src1 = std::to_string(a) + " + " + std::to_string(b);
    auto src2 = std::to_string(b) + " + " + std::to_string(a);

    auto expr1 = straylight::nix::compiler::parse::parse(src1, symbols1);
    auto expr2 = straylight::nix::compiler::parse::parse(src2, symbols2);

    straylight::nix::compiler::compile::compiler comp1(symbols1), comp2(symbols2);
    auto mod1 = comp1.compile(expr1);
    auto mod2 = comp2.compile(expr2);

    straylight::nix::compiler::runtime::wasm_executor exec1, exec2;
    auto res1 = exec1.execute(mod1.emit_binary());
    auto res2 = exec2.execute(mod2.emit_binary());

    RC_ASSERT(res1.success);
    RC_ASSERT(res2.success);
    RC_ASSERT(res1.value == res2.value);
  });
}

TEST_CASE("property: integer multiplication is commutative", "[property][execution]") {
  rc::check("a * b == b * a", []() {
    auto a = *rc::gen::inRange<std::int32_t>(-1000, 1000);
    auto b = *rc::gen::inRange<std::int32_t>(-1000, 1000);

    straylight::nix::compiler::ast::symbol_table symbols1, symbols2;
    auto src1 = std::to_string(a) + " * " + std::to_string(b);
    auto src2 = std::to_string(b) + " * " + std::to_string(a);

    auto expr1 = straylight::nix::compiler::parse::parse(src1, symbols1);
    auto expr2 = straylight::nix::compiler::parse::parse(src2, symbols2);

    straylight::nix::compiler::compile::compiler comp1(symbols1), comp2(symbols2);
    auto mod1 = comp1.compile(expr1);
    auto mod2 = comp2.compile(expr2);

    straylight::nix::compiler::runtime::wasm_executor exec1, exec2;
    auto res1 = exec1.execute(mod1.emit_binary());
    auto res2 = exec2.execute(mod2.emit_binary());

    RC_ASSERT(res1.success);
    RC_ASSERT(res2.success);
    RC_ASSERT(res1.value == res2.value);
  });
}

TEST_CASE("property: addition identity", "[property][execution]") {
  rc::check("a + 0 == a", []() {
    auto a = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = std::to_string(a) + " + 0";
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_int(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_int_value(res.value) == a);
  });
}

TEST_CASE("property: multiplication identity", "[property][execution]") {
  rc::check("a * 1 == a", []() {
    auto a = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = std::to_string(a) + " * 1";
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_int(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_int_value(res.value) == a);
  });
}

TEST_CASE("property: multiplication by zero", "[property][execution]") {
  rc::check("a * 0 == 0", []() {
    auto a = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = std::to_string(a) + " * 0";
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_int(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_int_value(res.value) == 0);
  });
}

// =============================================================================
// Boolean logic properties
// =============================================================================

TEST_CASE("property: double negation", "[property][execution]") {
  rc::check("!!b == b", [](bool b) {
    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = std::string("!!(") + (b ? "true" : "false") + ")";
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_bool(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_bool_value(res.value) == b);
  });
}

TEST_CASE("property: de morgan's law (and)", "[property][execution]") {
  rc::check("!(a && b) == (!a || !b)", [](bool a, bool b) {
    straylight::nix::compiler::ast::symbol_table symbols1, symbols2;
    auto as = a ? "true" : "false";
    auto bs = b ? "true" : "false";

    auto src1 = std::string("!(") + as + " && " + bs + ")";
    auto src2 = std::string("(!") + as + " || !" + bs + ")";

    auto expr1 = straylight::nix::compiler::parse::parse(src1, symbols1);
    auto expr2 = straylight::nix::compiler::parse::parse(src2, symbols2);

    straylight::nix::compiler::compile::compiler comp1(symbols1), comp2(symbols2);
    auto mod1 = comp1.compile(expr1);
    auto mod2 = comp2.compile(expr2);

    straylight::nix::compiler::runtime::wasm_executor exec1, exec2;
    auto res1 = exec1.execute(mod1.emit_binary());
    auto res2 = exec2.execute(mod2.emit_binary());

    RC_ASSERT(res1.success);
    RC_ASSERT(res2.success);
    RC_ASSERT(res1.value == res2.value);
  });
}

TEST_CASE("property: de morgan's law (or)", "[property][execution]") {
  rc::check("!(a || b) == (!a && !b)", [](bool a, bool b) {
    straylight::nix::compiler::ast::symbol_table symbols1, symbols2;
    auto as = a ? "true" : "false";
    auto bs = b ? "true" : "false";

    auto src1 = std::string("!(") + as + " || " + bs + ")";
    auto src2 = std::string("(!") + as + " && !" + bs + ")";

    auto expr1 = straylight::nix::compiler::parse::parse(src1, symbols1);
    auto expr2 = straylight::nix::compiler::parse::parse(src2, symbols2);

    straylight::nix::compiler::compile::compiler comp1(symbols1), comp2(symbols2);
    auto mod1 = comp1.compile(expr1);
    auto mod2 = comp2.compile(expr2);

    straylight::nix::compiler::runtime::wasm_executor exec1, exec2;
    auto res1 = exec1.execute(mod1.emit_binary());
    auto res2 = exec2.execute(mod2.emit_binary());

    RC_ASSERT(res1.success);
    RC_ASSERT(res2.success);
    RC_ASSERT(res1.value == res2.value);
  });
}

// =============================================================================
// Comparison properties
// =============================================================================

TEST_CASE("property: equality is reflexive", "[property][execution]") {
  rc::check("a == a is true", []() {
    auto a = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = std::to_string(a) + " == " + std::to_string(a);
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_bool(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_bool_value(res.value) == true);
  });
}

TEST_CASE("property: inequality is irreflexive", "[property][execution]") {
  rc::check("a != a is false", []() {
    auto a = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = std::to_string(a) + " != " + std::to_string(a);
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_bool(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_bool_value(res.value) == false);
  });
}

TEST_CASE("property: less than is irreflexive", "[property][execution]") {
  rc::check("a < a is false", []() {
    auto a = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = std::to_string(a) + " < " + std::to_string(a);
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_bool(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_bool_value(res.value) == false);
  });
}

TEST_CASE("property: less than or equal is reflexive", "[property][execution]") {
  rc::check("a <= a is true", []() {
    auto a = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = std::to_string(a) + " <= " + std::to_string(a);
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_bool(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_bool_value(res.value) == true);
  });
}

TEST_CASE("property: comparison trichotomy", "[property][execution]") {
  rc::check("exactly one of a < b, a == b, a > b is true", []() {
    auto a = *gen_safe_int();
    auto b = *gen_safe_int();

    auto check = [](const std::string& src) {
      straylight::nix::compiler::ast::symbol_table symbols;
      auto expr = straylight::nix::compiler::parse::parse(src, symbols);
      straylight::nix::compiler::compile::compiler comp(symbols);
      auto mod = comp.compile(expr);
      straylight::nix::compiler::runtime::wasm_executor exec;
      auto res = exec.execute(mod.emit_binary());
      return res.success && straylight::nix::compiler::runtime::is_bool(res.value) &&
             straylight::nix::compiler::runtime::get_bool_value(res.value);
    };

    auto as = std::to_string(a);
    auto bs = std::to_string(b);

    bool lt = check(as + " < " + bs);
    bool eq = check(as + " == " + bs);
    bool gt = check(as + " > " + bs);

    RC_ASSERT((lt ? 1 : 0) + (eq ? 1 : 0) + (gt ? 1 : 0) == 1);
  });
}

// =============================================================================
// Let binding properties
// =============================================================================

TEST_CASE("property: let binding shadows outer", "[property][execution]") {
  rc::check("inner let shadows outer", []() {
    auto outer = *gen_safe_int();
    auto inner = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src =
        "let x = " + std::to_string(outer) + "; in let x = " + std::to_string(inner) + "; in x";
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_int(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_int_value(res.value) == inner);
  });
}

// =============================================================================
// If expression properties
// =============================================================================

TEST_CASE("property: if true returns then branch", "[property][execution]") {
  rc::check("if true then a else b == a", []() {
    auto a = *gen_safe_int();
    auto b = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = "if true then " + std::to_string(a) + " else " + std::to_string(b);
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_int(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_int_value(res.value) == a);
  });
}

TEST_CASE("property: if false returns else branch", "[property][execution]") {
  rc::check("if false then a else b == b", []() {
    auto a = *gen_safe_int();
    auto b = *gen_safe_int();

    straylight::nix::compiler::ast::symbol_table symbols;
    auto src = "if false then " + std::to_string(a) + " else " + std::to_string(b);
    auto expr = straylight::nix::compiler::parse::parse(src, symbols);

    straylight::nix::compiler::compile::compiler comp(symbols);
    auto mod = comp.compile(expr);

    straylight::nix::compiler::runtime::wasm_executor exec;
    auto res = exec.execute(mod.emit_binary());

    RC_ASSERT(res.success);
    RC_ASSERT(straylight::nix::compiler::runtime::is_int(res.value));
    RC_ASSERT(straylight::nix::compiler::runtime::get_int_value(res.value) == b);
  });
}

// =============================================================================
// Symbol table properties
// =============================================================================

TEST_CASE("property: symbol table interning is idempotent", "[property][ast]") {
  rc::check("interning same string twice gives same symbol", []() {
    auto name = *gen_identifier();
    straylight::nix::compiler::ast::symbol_table symbols;

    auto s1 = symbols.intern(name);
    auto s2 = symbols.intern(name);

    RC_ASSERT(s1 == s2);
  });
}

TEST_CASE("property: symbol table lookup round-trips", "[property][ast]") {
  rc::check("lookup(intern(s)) == s", []() {
    auto name = *gen_identifier();
    straylight::nix::compiler::ast::symbol_table symbols;

    auto sym = symbols.intern(name);
    auto retrieved = symbols.lookup(sym);

    RC_ASSERT(retrieved == name);
  });
}

TEST_CASE("property: different strings get different symbols", "[property][ast]") {
  rc::check("different strings -> different symbols", []() {
    auto name1 = *gen_identifier();
    auto name2 = *gen_identifier();
    RC_PRE(name1 != name2); // precondition: names are different

    straylight::nix::compiler::ast::symbol_table symbols;
    auto s1 = symbols.intern(name1);
    auto s2 = symbols.intern(name2);

    RC_ASSERT(s1 != s2);
  });
}
