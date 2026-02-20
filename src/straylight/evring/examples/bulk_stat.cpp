// bulk_stat.cpp - Stat thousands of files efficiently using generator machines
//
// This example demonstrates generator machines for high-throughput I/O:
// - Generator machines proactively fill the submission queue
// - Orders of magnitude faster than sequential stat() calls
// - Still fully replayable for testing
//
// Usage: bulk_stat <directory> [count]

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <dirent.h>

#include "straylight/evring/evring.h"
#include "straylight/evring/generators.h"

// ============================================================================
// Helper: collect file paths from a directory
// ============================================================================

auto collect_paths(const char* dir, std::size_t max_count) -> std::vector<std::string> {
  std::vector<std::string> paths;
  paths.reserve(max_count);

  DIR* d = opendir(dir);
  if (!d) {
    std::fprintf(stderr, "Cannot open directory: %s\n", dir);
    return paths;
  }

  std::string prefix = dir;
  if (!prefix.empty() && prefix.back() != '/') {
    prefix += '/';
  }

  struct dirent* entry;
  while ((entry = readdir(d)) != nullptr && paths.size() < max_count) {
    // Skip . and ..
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }
    paths.push_back(prefix + entry->d_name);
  }
  closedir(d);

  return paths;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <directory> [count]\n", argv[0]);
    std::fprintf(stderr, "\nExample: %s /nix/store 10000\n", argv[0]);
    return 1;
  }

  const char* dir = argv[1];
  std::size_t count = argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 1000;

  // Collect file paths
  std::printf("Collecting up to %zu paths from %s...\n", count, dir);
  auto paths = collect_paths(dir, count);
  if (paths.empty()) {
    return 1;
  }
  std::printf("Found %zu files\n", paths.size());

  // Create io_uring ring with high depth for maximum parallelism
  auto ring = evring::make_io_uring_ring(256);
  if (!ring) {
    std::fprintf(stderr, "Failed to create io_uring\n");
    return 1;
  }

  // Convert to C-style path array for the generator
  std::vector<const char*> path_ptrs;
  path_ptrs.reserve(paths.size());
  for (const auto& p : paths) {
    path_ptrs.push_back(p.c_str());
  }

  // Allocate statx buffers
  std::vector<struct statx> statx_buffers(paths.size());

  // Create generator machine
  evring::bulk_stat_machine statter{std::span{path_ptrs.data(), path_ptrs.size()},
                                    evring::make_stable_span(statx_buffers)};

  // Time the operation
  std::printf("Stating %zu files with generator machine...\n", paths.size());
  auto start = std::chrono::steady_clock::now();

  // Run the generator - this proactively keeps the SQ full
  auto state = evring::run_generate(statter, *ring);

  auto end = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  double elapsed_s = elapsed_ms / 1000.0;

  // Report results
  std::printf("\nResults:\n");
  std::printf("  Files processed: %zu\n", state.completed);
  std::printf("  Failed:          %zu\n", state.failed);
  std::printf("  Time:            %.3f seconds\n", elapsed_s);
  if (elapsed_s > 0) {
    std::printf("  Throughput:      %.0f ops/sec\n", state.completed / elapsed_s);
  }

  // Show some example results
  std::printf("\nFirst 5 files:\n");
  for (std::size_t i = 0; i < std::min(paths.size(), std::size_t{5}); ++i) {
    auto& st = statx_buffers[i];
    if (st.stx_mask & STATX_SIZE) {
      std::printf("  %s: %llu bytes, mode=%o\n", paths[i].c_str(),
                  static_cast<unsigned long long>(st.stx_size), st.stx_mode & 0777);
    } else {
      std::printf("  %s: (stat failed)\n", paths[i].c_str());
    }
  }

  // Compare with POSIX baseline
  std::printf("\nComparing with POSIX stat()...\n");
  start = std::chrono::steady_clock::now();

  std::size_t posix_ok = 0;
  for (std::size_t i = 0; i < paths.size(); ++i) {
    struct statx st;
    if (statx(AT_FDCWD, paths[i].c_str(), 0, STATX_BASIC_STATS, &st) == 0) {
      ++posix_ok;
    }
  }

  end = std::chrono::steady_clock::now();
  elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  elapsed_s = elapsed_ms / 1000.0;

  std::printf("  POSIX stat():    %.3f seconds (%.0f ops/sec)\n", elapsed_s, posix_ok / elapsed_s);
  std::printf("  Speedup:         %.1fx\n",
              (posix_ok / elapsed_s) > 0 ? (state.completed / elapsed_s) / (posix_ok / elapsed_s)
                                         : 0);

  return 0;
}
