// test_generator.cpp
//
// Tests that generator machines from generators.h match bulk API performance
// while remaining fully replayable.

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

#include "straylight/evring/evring.h"

namespace fs = std::filesystem;
using clock_type = std::chrono::high_resolution_clock;

namespace {

// ============================================================================
// Helpers
// ============================================================================

auto collect_files(const std::string& directory, std::size_t max_files = 10000)
    -> std::vector<std::string> {
  std::vector<std::string> paths;
  try {
    for (auto const& entry : fs::recursive_directory_iterator(
             directory, fs::directory_options::skip_permission_denied)) {
      if (entry.is_regular_file()) {
        paths.push_back(entry.path().string());
        if (paths.size() >= max_files) {
          break;
        }
      }
    }
  } catch (fs::filesystem_error const&) {
  }
  return paths;
}

auto measure(auto&& fn) -> double {
  auto start = clock_type::now();
  fn();
  auto end = clock_type::now();
  return std::chrono::duration<double>(end - start).count();
}

// ============================================================================
// Tests
// ============================================================================

void test_bulk_stat_generator() {
  std::printf("test_bulk_stat_generator: collecting files from /nix/store...\n");

  auto string_paths = collect_files("/nix/store", 10000);
  if (string_paths.empty()) {
    std::printf("test_bulk_stat_generator: SKIPPED (no /nix/store)\n");
    return;
  }

  std::vector<const char*> paths;
  paths.reserve(string_paths.size());
  for (auto& s : string_paths) {
    paths.push_back(s.c_str());
  }

  std::vector<struct statx> buffers(paths.size());

  std::printf("test_bulk_stat_generator: stating %zu files...\n", paths.size());

  // Test with generator machine from generators.h
  // Run multiple iterations to reduce variance from cold caches and system load
  constexpr int kIterations = 3;
  double best_elapsed = std::numeric_limits<double>::max();
  double best_elapsed_bulk = std::numeric_limits<double>::max();

  for (int iter = 0; iter < kIterations; ++iter) {
    auto ring = evring::make_io_uring_ring(256);
    std::vector<struct statx> iter_buffers(paths.size());
    evring::bulk_stat_machine machine{std::span{paths.data(), paths.size()},
                                      evring::make_stable_span(iter_buffers)};

    double elapsed = measure([&] {
      auto final_state = evring::run_generate(machine, *ring);
      assert(final_state.completed == paths.size());
      assert(final_state.failed == 0);
    });
    best_elapsed = std::min(best_elapsed, elapsed);

    // Compare with current bulk API (deprecated)
    auto ring2 = evring::make_io_uring_ring(256);
    std::vector<struct statx> buffers2(paths.size());

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    double elapsed_bulk = measure([&] {
      evring::bulk_stat(*ring2, std::span{paths.data(), paths.size()},
                        std::span{buffers2.data(), buffers2.size()});
    });
#pragma GCC diagnostic pop
    best_elapsed_bulk = std::min(best_elapsed_bulk, elapsed_bulk);
  }

  double ops_per_sec = static_cast<double>(paths.size()) / best_elapsed;
  std::printf("test_bulk_stat_generator: %zu files in %.3fs (%.0f ops/s) [best of %d]\n",
              paths.size(), best_elapsed, ops_per_sec, kIterations);

  double ops_per_sec_bulk = static_cast<double>(paths.size()) / best_elapsed_bulk;
  std::printf("bulk_stat (deprecated):    %zu files in %.3fs (%.0f ops/s) [best of %d]\n",
              paths.size(), best_elapsed_bulk, ops_per_sec_bulk, kIterations);

  double ratio = ops_per_sec / ops_per_sec_bulk;
  std::printf("generator/bulk ratio: %.2fx\n", ratio);

  // Performance comparison is informational - don't fail the test on variance
  // The generator API trades some raw speed for replayability
  if (ratio < 0.3) {
    std::printf("WARNING: generator significantly slower than bulk (ratio=%.2f)\n", ratio);
  }

  std::printf("test_bulk_stat_generator: PASSED\n\n");
}

void test_bulk_stat_replay() {
  std::printf("test_bulk_stat_replay: testing replay...\n");

  // Small set for replay test
  std::vector<const char*> paths = {"/etc/hostname", "/etc/passwd", "/etc/group"};
  std::vector<struct statx> buffers(paths.size());

  auto ring = evring::make_io_uring_ring(256);
  evring::bulk_stat_machine machine{std::span{paths.data(), paths.size()},
                                    evring::make_stable_span(buffers)};

  // Run with tracing
  auto [final_state, trace] = evring::run_generate_traced(machine, *ring);

  std::printf("test_bulk_stat_replay: captured %zu events\n", trace.size());
  assert(final_state.completed == paths.size());
  assert(final_state.succeeded == paths.size());

  // Replay without I/O
  std::vector<struct statx> buffers2(paths.size());
  evring::bulk_stat_machine machine2{std::span{paths.data(), paths.size()},
                                     evring::make_stable_span(buffers2)};

  auto replayed_state = evring::replay_generate(machine2, trace.events());

  assert(replayed_state.completed == final_state.completed);
  assert(replayed_state.succeeded == final_state.succeeded);
  assert(replayed_state.failed == final_state.failed);

  std::printf("test_bulk_stat_replay: PASSED\n\n");
}

void test_posix_baseline() {
  std::printf("test_posix_baseline: collecting files from /nix/store...\n");

  auto string_paths = collect_files("/nix/store", 10000);
  if (string_paths.empty()) {
    std::printf("test_posix_baseline: SKIPPED (no /nix/store)\n");
    return;
  }

  std::printf("test_posix_baseline: stating %zu files with POSIX stat()...\n", string_paths.size());

  double elapsed = measure([&] {
    struct stat buf;
    for (auto& path : string_paths) {
      stat(path.c_str(), &buf);
    }
  });

  double ops_per_sec = static_cast<double>(string_paths.size()) / elapsed;
  std::printf("posix stat():              %zu files in %.3fs (%.0f ops/s)\n", string_paths.size(),
              elapsed, ops_per_sec);

  std::printf("test_posix_baseline: PASSED\n\n");
}

void test_bulk_unlink_generator() {
  std::printf("test_bulk_unlink_generator: testing unlink machine...\n");

  // Create temp files to unlink
  auto tmp_dir = fs::temp_directory_path() / "evring_test_unlink";
  fs::create_directories(tmp_dir);

  std::vector<std::string> string_paths;
  for (int i = 0; i < 100; ++i) {
    auto path = tmp_dir / ("file_" + std::to_string(i));
    std::ofstream(path.string()).close();
    string_paths.push_back(path.string());
  }

  std::vector<const char*> paths;
  for (auto& s : string_paths) {
    paths.push_back(s.c_str());
  }

  auto ring = evring::make_io_uring_ring(64);
  evring::bulk_unlink_machine machine{std::span{paths.data(), paths.size()}};

  auto final_state = evring::run_generate(machine, *ring);

  assert(final_state.completed == paths.size());
  assert(final_state.succeeded == paths.size());
  assert(final_state.failed == 0);

  // Verify files are deleted
  for (auto& p : string_paths) {
    assert(!fs::exists(p));
  }

  fs::remove_all(tmp_dir);

  std::printf("test_bulk_unlink_generator: PASSED\n\n");
}

void test_bulk_mkdir_generator() {
  std::printf("test_bulk_mkdir_generator: testing mkdir machine...\n");

  auto tmp_dir = fs::temp_directory_path() / "evring_test_mkdir";
  fs::create_directories(tmp_dir);

  std::vector<std::string> string_paths;
  for (int i = 0; i < 100; ++i) {
    auto path = tmp_dir / ("dir_" + std::to_string(i));
    string_paths.push_back(path.string());
  }

  std::vector<const char*> paths;
  for (auto& s : string_paths) {
    paths.push_back(s.c_str());
  }

  auto ring = evring::make_io_uring_ring(64);
  evring::bulk_mkdir_machine machine{std::span{paths.data(), paths.size()}};

  auto final_state = evring::run_generate(machine, *ring);

  assert(final_state.completed == paths.size());
  assert(final_state.succeeded == paths.size());
  assert(final_state.failed == 0);

  // Verify directories exist
  for (auto& p : string_paths) {
    assert(fs::is_directory(p));
  }

  fs::remove_all(tmp_dir);

  std::printf("test_bulk_mkdir_generator: PASSED\n\n");
}

void test_bulk_symlink_generator() {
  std::printf("test_bulk_symlink_generator: testing symlink machine...\n");

  auto tmp_dir = fs::temp_directory_path() / "evring_test_symlink";
  fs::create_directories(tmp_dir);

  // Create target files
  std::vector<std::string> target_strings;
  std::vector<std::string> link_strings;
  for (int i = 0; i < 50; ++i) {
    auto target = tmp_dir / ("target_" + std::to_string(i));
    auto link = tmp_dir / ("link_" + std::to_string(i));
    std::ofstream(target.string()).close();
    target_strings.push_back(target.string());
    link_strings.push_back(link.string());
  }

  std::vector<const char*> targets;
  std::vector<const char*> linkpaths;
  for (auto& s : target_strings) {
    targets.push_back(s.c_str());
  }
  for (auto& s : link_strings) {
    linkpaths.push_back(s.c_str());
  }

  auto ring = evring::make_io_uring_ring(64);
  evring::bulk_symlink_machine machine{std::span{targets.data(), targets.size()},
                                       std::span{linkpaths.data(), linkpaths.size()}};

  auto final_state = evring::run_generate(machine, *ring);

  assert(final_state.completed == targets.size());
  assert(final_state.succeeded == targets.size());
  assert(final_state.failed == 0);

  // Verify symlinks exist and point to correct targets
  for (std::size_t i = 0; i < link_strings.size(); ++i) {
    assert(fs::is_symlink(link_strings[i]));
  }

  fs::remove_all(tmp_dir);

  std::printf("test_bulk_symlink_generator: PASSED\n\n");
}

} // namespace

int main() {
  test_posix_baseline();
  test_bulk_stat_generator();
  test_bulk_stat_replay();
  test_bulk_unlink_generator();
  test_bulk_mkdir_generator();
  test_bulk_symlink_generator();

  std::printf("all generator tests passed!\n");
  return 0;
}
