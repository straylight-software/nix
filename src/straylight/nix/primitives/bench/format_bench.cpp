// straylight::nix::primitives::format benchmarks
//
// Benchmarks comparing std::format vs boost::format performance.

#include <cstdint>
#include <string>

#include <boost/format.hpp>

#include "../format.h"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace fmt = straylight::nix::primitives;

namespace {

// Sink to prevent optimization
volatile const char* g_sink = nullptr;

void consume(const std::string& s) {
  g_sink = s.c_str();
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Format Benchmarks").warmup(1000).minEpochIterations(10000).unit("op");

  // ───────────────────────────────────────────────────────────────────────────
  // Simple string formatting
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::format - no args", [] {
    auto s = fmt::format("hello world");
    consume(s);
  });

  bench.run("boost::format - no args", [] {
    auto s = (boost::format("hello world")).str();
    consume(s);
  });

  bench.run("std::format - one string", [] {
    auto s = fmt::format("hello {}", "world");
    consume(s);
  });

  bench.run("boost::format - one string %s", [] {
    auto s = (boost::format("hello %s") % "world").str();
    consume(s);
  });

  bench.run("fmt() compat - one string %s", [] {
    auto s = fmt::fmt("hello %s", "world");
    consume(s);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Multiple arguments
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::format - 3 args", [] {
    auto s = fmt::format("{} + {} = {}", 1, 2, 3);
    consume(s);
  });

  bench.run("boost::format - 3 args %d", [] {
    auto s = (boost::format("%d + %d = %d") % 1 % 2 % 3).str();
    consume(s);
  });

  bench.run("fmt() compat - 3 args %d", [] {
    auto s = fmt::fmt("%d + %d = %d", 1, 2, 3);
    consume(s);
  });

  bench.run("boost::format - 3 args %1%", [] {
    auto s = (boost::format("%1% + %2% = %3%") % 1 % 2 % 3).str();
    consume(s);
  });

  bench.run("fmt() compat - 3 args %1%", [] {
    auto s = fmt::fmt("%1% + %2% = %3%", 1, 2, 3);
    consume(s);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Mixed types
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::format - mixed types", [] {
    auto s = fmt::format("path: {}, line: {}, col: {}", "/nix/store/abc", 42, 17);
    consume(s);
  });

  bench.run("boost::format - mixed types", [] {
    auto s = (boost::format("path: %s, line: %d, col: %d") % "/nix/store/abc" % 42 % 17).str();
    consume(s);
  });

  bench.run("fmt() compat - mixed types", [] {
    auto s = fmt::fmt("path: %s, line: %d, col: %d", "/nix/store/abc", 42, 17);
    consume(s);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Longer format strings (more realistic)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::format - long string", [] {
    auto s = fmt::format("error: {} at {}:{}: expected {}, got {} (in context {})", "syntax error",
                         "/nix/store/abc123-source/default.nix", 42, "identifier", "string literal",
                         "function definition");
    consume(s);
  });

  bench.run("boost::format - long string", [] {
    auto s = (boost::format("error: %s at %s:%d: expected %s, got %s (in context %s)") %
              "syntax error" % "/nix/store/abc123-source/default.nix" % 42 % "identifier" %
              "string literal" % "function definition")
                 .str();
    consume(s);
  });

  bench.run("fmt() compat - long string", [] {
    auto s = fmt::fmt("error: %s at %s:%d: expected %s, got %s (in context %s)", "syntax error",
                      "/nix/store/abc123-source/default.nix", 42, "identifier", "string literal",
                      "function definition");
    consume(s);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Hex formatting
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::format - hex", [] {
    auto s = fmt::format("{:08x}", 0xDEADBEEF);
    consume(s);
  });

  bench.run("boost::format - hex", [] {
    auto s = (boost::format("%08x") % 0xDEADBEEF).str();
    consume(s);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Color wrappers (HintFmt replacement)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::format - Magenta wrapper", [] {
    auto s = fmt::format("expected {}, got {}", fmt::Magenta("foo"), fmt::Magenta("bar"));
    consume(s);
  });

  bench.run("Hint class construction", [] {
    auto h = fmt::Hint("expected {}, got {}", fmt::Magenta("foo"), fmt::Magenta("bar"));
    consume(h.str());
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Escape sequences
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("boost::format - %% escape", [] {
    auto s = (boost::format("100%% complete")).str();
    consume(s);
  });

  bench.run("fmt() compat - %% escape", [] {
    auto s = fmt::fmt("100%% complete");
    consume(s);
  });

  bench.run("std::format - no escape needed", [] {
    auto s = fmt::format("100% complete");
    consume(s);
  });

  return 0;
}
