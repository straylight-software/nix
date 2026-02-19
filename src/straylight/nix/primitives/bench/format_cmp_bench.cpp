// straylight::nix::primitives::bench::format_cmp_bench
//
// Comprehensive comparison benchmarks: straylight format.h vs boost::format
//
// This benchmark compares:
//   - std::format (via straylight::format) vs boost::format
//   - fmt() compatibility layer vs boost::format
//   - Various format patterns common in nix codebase
//
// Expected speedups:
//   - Simple formatting: 5-20x faster (std::format compile-time parsing)
//   - Multiple arguments: 10-30x faster (no runtime parsing overhead)
//   - Hex/numeric formatting: 3-10x faster
//
// Why std::format is faster than boost::format:
//   - Compile-time format string parsing (vs runtime parsing)
//   - No intermediate format object allocation
//   - Better inlining opportunities
//   - Modern allocator-aware string building

#define ANKERL_NANOBENCH_IMPLEMENT
#include <cstdint>
#include <iostream>
#include <string>

#include <nanobench.h>

#include <boost/format.hpp>

// Straylight primitives
#include "../format.h"

namespace fmt = straylight::nix::primitives;

namespace {

// Sink to prevent optimization
volatile const char* g_sink = nullptr;

void consume(const std::string& s) {
  g_sink = s.c_str();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: No arguments (passthrough)
// ─────────────────────────────────────────────────────────────────────────────

void bench_no_args(ankerl::nanobench::Bench& b) {
  b.run("boost::format/no_args", [] {
    auto s = (boost::format("hello world")).str();
    consume(s);
  });

  b.run("straylight::format/no_args", [] {
    auto s = fmt::format("hello world");
    consume(s);
  });

  // Expected: straylight ~10x faster (no format object allocation)
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Single string argument
// ─────────────────────────────────────────────────────────────────────────────

void bench_single_string(ankerl::nanobench::Bench& b) {
  // boost::format %s style
  b.run("boost::format/%s", [] {
    auto s = (boost::format("hello %s!") % "world").str();
    consume(s);
  });

  // boost::format %1% positional style
  b.run("boost::format/%1%", [] {
    auto s = (boost::format("hello %1%!") % "world").str();
    consume(s);
  });

  // straylight::format (std::format syntax)
  b.run("straylight::format/{}", [] {
    auto s = fmt::format("hello {}!", "world");
    consume(s);
  });

  // straylight::fmt() compatibility layer
  b.run("straylight::fmt/%s", [] {
    auto s = fmt::fmt("hello %s!", "world");
    consume(s);
  });

  b.run("straylight::fmt/%1%", [] {
    auto s = fmt::fmt("hello %1%!", "world");
    consume(s);
  });

  // Expected: straylight::format ~15x faster, fmt() ~5x faster (runtime conversion)
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Multiple integer arguments
// ─────────────────────────────────────────────────────────────────────────────

void bench_multi_int(ankerl::nanobench::Bench& b) {
  b.run("boost::format/3_ints/%d", [] {
    auto s = (boost::format("%d + %d = %d") % 1 % 2 % 3).str();
    consume(s);
  });

  b.run("boost::format/3_ints/%1%", [] {
    auto s = (boost::format("%1% + %2% = %3%") % 1 % 2 % 3).str();
    consume(s);
  });

  b.run("straylight::format/3_ints/{}", [] {
    auto s = fmt::format("{} + {} = {}", 1, 2, 3);
    consume(s);
  });

  b.run("straylight::fmt/3_ints/%d", [] {
    auto s = fmt::fmt("%d + %d = %d", 1, 2, 3);
    consume(s);
  });

  // Expected: straylight::format ~20x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Mixed types (common error message pattern)
// ─────────────────────────────────────────────────────────────────────────────

void bench_mixed_types(ankerl::nanobench::Bench& b) {
  b.run("boost::format/mixed", [] {
    auto s = (boost::format("path: %s, line: %d, col: %d") % "/nix/store/abc" % 42 % 17).str();
    consume(s);
  });

  b.run("straylight::format/mixed", [] {
    auto s = fmt::format("path: {}, line: {}, col: {}", "/nix/store/abc", 42, 17);
    consume(s);
  });

  b.run("straylight::fmt/mixed", [] {
    auto s = fmt::fmt("path: %s, line: %d, col: %d", "/nix/store/abc", 42, 17);
    consume(s);
  });

  // Expected: straylight::format ~15x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Long format strings (realistic error messages)
// ─────────────────────────────────────────────────────────────────────────────

void bench_long_format(ankerl::nanobench::Bench& b) {
  b.run("boost::format/long_error", [] {
    auto s = (boost::format("error: %s at %s:%d: expected %s, got %s (in context %s)") %
              "syntax error" % "/nix/store/abc123-source/default.nix" % 42 % "identifier" %
              "string literal" % "function definition")
                 .str();
    consume(s);
  });

  b.run("straylight::format/long_error", [] {
    auto s = fmt::format("error: {} at {}:{}: expected {}, got {} (in context {})", "syntax error",
                         "/nix/store/abc123-source/default.nix", 42, "identifier", "string literal",
                         "function definition");
    consume(s);
  });

  b.run("straylight::fmt/long_error", [] {
    auto s = fmt::fmt("error: %s at %s:%d: expected %s, got %s (in context %s)", "syntax error",
                      "/nix/store/abc123-source/default.nix", 42, "identifier", "string literal",
                      "function definition");
    consume(s);
  });

  // Expected: straylight::format ~10x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Hexadecimal formatting (hash display)
// ─────────────────────────────────────────────────────────────────────────────

void bench_hex_format(ankerl::nanobench::Bench& b) {
  b.run("boost::format/hex/%08x", [] {
    auto s = (boost::format("%08x") % 0xDEADBEEF).str();
    consume(s);
  });

  b.run("straylight::format/hex/{:08x}", [] {
    auto s = fmt::format("{:08x}", 0xDEADBEEF);
    consume(s);
  });

  // Multiple hex values (like displaying a hash)
  b.run("boost::format/multi_hex", [] {
    auto s = (boost::format("%08x%08x%08x%08x") % 0xDEADBEEF % 0xCAFEBABE % 0x12345678 % 0x87654321)
                 .str();
    consume(s);
  });

  b.run("straylight::format/multi_hex", [] {
    auto s =
        fmt::format("{:08x}{:08x}{:08x}{:08x}", 0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x87654321);
    consume(s);
  });

  // Expected: straylight ~5-8x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Store path formatting (very common in nix)
// ─────────────────────────────────────────────────────────────────────────────

void bench_store_path_format(ankerl::nanobench::Bench& b) {
  const char* hash = "3b4p38fk6hgwaf2p6jcci5k93b9cnxc8";
  const char* name = "gcc-15.2.0";

  b.run("boost::format/store_path", [&] {
    auto s = (boost::format("/nix/store/%s-%s") % hash % name).str();
    consume(s);
  });

  b.run("straylight::format/store_path", [&] {
    auto s = fmt::format("/nix/store/{}-{}", hash, name);
    consume(s);
  });

  // With derivation output
  b.run("boost::format/drv_output", [&] {
    auto s = (boost::format("/nix/store/%s-%s.drv!out") % hash % name).str();
    consume(s);
  });

  b.run("straylight::format/drv_output", [&] {
    auto s = fmt::format("/nix/store/{}-{}.drv!out", hash, name);
    consume(s);
  });

  // Expected: straylight ~10x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Escape sequences
// ─────────────────────────────────────────────────────────────────────────────

void bench_escapes(ankerl::nanobench::Bench& b) {
  b.run("boost::format/%%_escape", [] {
    auto s = (boost::format("100%% complete, 50%% remaining")).str();
    consume(s);
  });

  b.run("straylight::fmt/%%_escape", [] {
    auto s = fmt::fmt("100%% complete, 50%% remaining");
    consume(s);
  });

  // std::format doesn't need %% escaping
  b.run("straylight::format/no_escape", [] {
    auto s = fmt::format("100% complete, 50% remaining");
    consume(s);
  });

  // Expected: straylight::format much faster (no escape processing)
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Color wrapper formatting (HintFmt replacement)
// ─────────────────────────────────────────────────────────────────────────────

void bench_color_format(ankerl::nanobench::Bench& b) {
  b.run("straylight::Magenta_wrapper", [] {
    auto s = fmt::format("expected {}, got {}", fmt::Magenta("foo"), fmt::Magenta("bar"));
    consume(s);
  });

  b.run("straylight::Hint_class", [] {
    auto h = fmt::Hint("expected {}, got {}", fmt::Magenta("foo"), fmt::Magenta("bar"));
    consume(h.str());
  });

  // Manual ANSI code insertion (old nix style)
  b.run("boost::format/manual_ansi", [] {
    auto s =
        (boost::format("expected \033[35;1m%s\033[0m, got \033[35;1m%s\033[0m") % "foo" % "bar")
            .str();
    consume(s);
  });

  // Expected: straylight Hint similar speed to Magenta wrapper
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Repeated formatting (simulates hot loop)
// ─────────────────────────────────────────────────────────────────────────────

void bench_repeated(ankerl::nanobench::Bench& b) {
  b.run("boost::format/repeated_1000", [] {
    for (int i = 0; i < 1000; ++i) {
      auto s = (boost::format("iteration %d: value = %d") % i % (i * 2)).str();
      ankerl::nanobench::doNotOptimizeAway(s);
    }
  });

  b.run("straylight::format/repeated_1000", [] {
    for (int i = 0; i < 1000; ++i) {
      auto s = fmt::format("iteration {}: value = {}", i, i * 2);
      ankerl::nanobench::doNotOptimizeAway(s);
    }
  });

  // Expected: straylight ~15-20x faster in aggregate
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "Format Comparison: boost::format vs straylight format.h (std::format)\n";
  std::cout << "======================================================================\n";
  std::cout << "\n";
  std::cout << "Expected speedups:\n";
  std::cout << "  - Simple: 5-20x (compile-time parsing)\n";
  std::cout << "  - Multiple args: 10-30x (no runtime overhead)\n";
  std::cout << "  - Hex/numeric: 3-10x\n";
  std::cout << "  - fmt() compat: 3-8x (runtime conversion overhead)\n";
  std::cout << "\n";

  ankerl::nanobench::Bench b;
  b.title("Format Comparison").warmup(1000).minEpochIterations(10000).unit("op");
  b.relative(true);

  std::cout << "=== No Arguments ===\n";
  bench_no_args(b);

  std::cout << "\n=== Single String Argument ===\n";
  bench_single_string(b);

  std::cout << "\n=== Multiple Integer Arguments ===\n";
  bench_multi_int(b);

  std::cout << "\n=== Mixed Types ===\n";
  bench_mixed_types(b);

  std::cout << "\n=== Long Format Strings ===\n";
  bench_long_format(b);

  std::cout << "\n=== Hexadecimal Formatting ===\n";
  bench_hex_format(b);

  std::cout << "\n=== Store Path Formatting ===\n";
  bench_store_path_format(b);

  std::cout << "\n=== Escape Sequences ===\n";
  bench_escapes(b);

  std::cout << "\n=== Color/Hint Formatting ===\n";
  bench_color_format(b);

  std::cout << "\n=== Repeated Formatting (Hot Loop) ===\n";
  bench_repeated(b);

  return 0;
}
