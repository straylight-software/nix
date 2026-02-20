// straylight::nix::url::bench::url_bench
//
// Microbenchmarks comparing URL parsing backends:
//   - boost::url (RFC 3986)
//   - ada (WHATWG)
//   - straylight::nix::primitives (wrapper)
//
// Uses ankerl::nanobench for high-quality microbenchmarking

#define ANKERL_NANOBENCH_IMPLEMENT
#include <iostream>
#include <string>
#include <vector>

#include <nanobench.h>

#include "url_shim.h"

namespace bench = straylight::nix::url::bench;

// ─────────────────────────────────────────────────────────────────────────────
// Single URL parsing benchmarks
// ─────────────────────────────────────────────────────────────────────────────

void bench_single_urls(ankerl::nanobench::Bench& b) {
  // Test each URL category individually
  const std::vector<std::pair<bench::url_category, const char*>> categories = {
      {bench::url_category::github_https, "github_https"},
      {bench::url_category::http_cache, "http_cache"},
      {bench::url_category::file_local, "file_local"},
      {bench::url_category::s3_bucket, "s3_bucket"},
      {bench::url_category::ipv6_literal, "ipv6_literal"},
      {bench::url_category::long_query, "long_query"},
      {bench::url_category::deep_path, "deep_path"},
  };

  for (const auto& [cat, name] : categories) {
    std::string url = bench::generate_url(cat);

    b.run(std::string("boost/") + name, [&] {
      auto result = bench::boost_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    b.run(std::string("ada/") + name, [&] {
      auto result = bench::ada_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    b.run(std::string("straylight/") + name, [&] {
      auto result = bench::straylight_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    b.run(std::string("straylight_fast/") + name, [&] {
      auto result = bench::straylight_fast_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Validation-only benchmarks (can_parse)
// ─────────────────────────────────────────────────────────────────────────────

void bench_validation(ankerl::nanobench::Bench& b) {
  auto urls = bench::workloads::mixed_realistic();

  b.run("boost/can_parse", [&] {
    for (const auto& url : urls) {
      ankerl::nanobench::doNotOptimizeAway(bench::boost_backend::can_parse(url));
    }
  });

  b.run("ada/can_parse", [&] {
    for (const auto& url : urls) {
      ankerl::nanobench::doNotOptimizeAway(bench::ada_backend::can_parse(url));
    }
  });

  b.run("straylight/can_parse", [&] {
    for (const auto& url : urls) {
      ankerl::nanobench::doNotOptimizeAway(bench::straylight_backend::can_parse(url));
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Workload benchmarks (realistic scenarios)
// ─────────────────────────────────────────────────────────────────────────────

void bench_workload(ankerl::nanobench::Bench& b, const std::vector<std::string>& urls,
                    const char* workload_name) {
  b.run(std::string("boost/") + workload_name, [&] {
    for (const auto& url : urls) {
      auto result = bench::boost_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  b.run(std::string("ada/") + workload_name, [&] {
    for (const auto& url : urls) {
      auto result = bench::ada_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  b.run(std::string("straylight/") + workload_name, [&] {
    for (const auto& url : urls) {
      auto result = bench::straylight_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });
}

void bench_workloads(ankerl::nanobench::Bench& b) {
  // Pre-generate workloads
  auto nix_build = bench::workloads::nix_build();
  auto flake_eval = bench::workloads::flake_eval();
  auto cache_ops = bench::workloads::cache_ops();
  auto edge_cases = bench::workloads::edge_cases();
  auto mixed = bench::workloads::mixed_realistic();

  bench_workload(b, nix_build, "nix_build");
  bench_workload(b, flake_eval, "flake_eval");
  bench_workload(b, cache_ops, "cache_ops");
  bench_workload(b, edge_cases, "edge_cases");
  bench_workload(b, mixed, "mixed_realistic");
}

// ─────────────────────────────────────────────────────────────────────────────
// Throughput benchmark (URLs per second)
// ─────────────────────────────────────────────────────────────────────────────

void bench_throughput(ankerl::nanobench::Bench& b) {
  auto urls = bench::workloads::mixed_realistic();
  const std::size_t n = urls.size();

  b.batch(n).unit("URL").run("boost/throughput", [&] {
    for (const auto& url : urls) {
      auto result = bench::boost_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  b.batch(n).unit("URL").run("ada/throughput", [&] {
    for (const auto& url : urls) {
      auto result = bench::ada_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  b.batch(n).unit("URL").run("straylight/throughput", [&] {
    for (const auto& url : urls) {
      auto result = bench::straylight_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  b.batch(n).unit("URL").run("straylight_fast/throughput", [&] {
    for (const auto& url : urls) {
      auto result = bench::straylight_fast_backend::parse(url);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  std::cout << "=== URL Parser Benchmark ===" << std::endl;
  std::cout << "Backends: boost::url (RFC 3986), ada (WHATWG), straylight (wrapper)" << std::endl;
  std::cout << std::endl;

  // Determine which benchmarks to run
  bool run_single = true;
  bool run_validation = true;
  bool run_workloads = true;
  bool run_throughput = true;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--single") {
      run_single = true;
      run_validation = run_workloads = run_throughput = false;
    } else if (arg == "--validation") {
      run_validation = true;
      run_single = run_workloads = run_throughput = false;
    } else if (arg == "--workloads") {
      run_workloads = true;
      run_single = run_validation = run_throughput = false;
    } else if (arg == "--throughput") {
      run_throughput = true;
      run_single = run_validation = run_workloads = false;
    } else if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
      std::cout << "Options:" << std::endl;
      std::cout << "  --single       Run single URL parsing benchmarks" << std::endl;
      std::cout << "  --validation   Run validation-only benchmarks" << std::endl;
      std::cout << "  --workloads    Run workload benchmarks" << std::endl;
      std::cout << "  --throughput   Run throughput benchmarks" << std::endl;
      std::cout << "  (default: run all)" << std::endl;
      return 0;
    }
  }

  ankerl::nanobench::Bench bench;
  bench.title("URL Parsing").warmup(100).relative(true).minEpochIterations(1000);

  if (run_single) {
    std::cout << "--- Single URL Parsing ---" << std::endl;
    bench_single_urls(bench);
    std::cout << std::endl;
  }

  if (run_validation) {
    std::cout << "--- Validation Only (can_parse) ---" << std::endl;
    bench_validation(bench);
    std::cout << std::endl;
  }

  if (run_workloads) {
    std::cout << "--- Workload Benchmarks ---" << std::endl;
    bench_workloads(bench);
    std::cout << std::endl;
  }

  if (run_throughput) {
    std::cout << "--- Throughput (URLs/sec) ---" << std::endl;
    bench_throughput(bench);
    std::cout << std::endl;
  }

  return 0;
}
