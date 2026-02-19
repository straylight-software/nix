// test_generator.cpp
//
// Proves that generator machines match bulk API performance
// while remaining fully replayable.

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

#include "evring/evring.h"

namespace fs = std::filesystem;
using clock_type = std::chrono::high_resolution_clock;

namespace {

// ============================================================================
// bulk_stat as a generator machine
// ============================================================================

struct bulk_stat_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors;
};

struct bulk_stat_machine {
  using state_type = bulk_stat_state;

  std::span<const char* const> paths;
  std::span<struct statx> buffers;
  unsigned int mask;

  bulk_stat_machine(std::span<const char* const> p, std::span<struct statx> b,
                    unsigned int m = STATX_BASIC_STATS)
      : paths(p), buffers(b), mask(m) {}

  auto initial() const -> state_type { return {}; }

  auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < paths.size();
  }

  auto generate(state_type s, std::size_t max_ops) const -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;
    ops.reserve(std::min(max_ops, paths.size() - s.next_to_submit));

    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      ops.push_back(evring::operation::make_statx(AT_FDCWD, paths[s.next_to_submit], 0, mask,
                                                  &buffers[s.next_to_submit], s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  auto step(state_type s, evring::event const& e) const -> evring::step_result<state_type> {
    s.completed++;
    if (e.result >= 0) {
      s.succeeded++;
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  auto done(const state_type& s) const -> bool { return s.completed >= paths.size(); }
};

// ============================================================================
// bulk_create as a generator machine
// ============================================================================

struct bulk_create_state {
  std::size_t next_to_submit{0};
  std::size_t opened{0};
  std::size_t closed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> fds_to_close;
  std::vector<int> errors;
};

struct bulk_create_machine {
  using state_type = bulk_create_state;

  std::span<const char* const> paths;
  mode_t mode;

  bulk_create_machine(std::span<const char* const> p, mode_t m = 0644) : paths(p), mode(m) {}

  auto initial() const -> state_type { return {}; }

  auto wants_to_submit(const state_type& s) const -> bool {
    // want to submit if we have files to open OR files to close
    return s.next_to_submit < paths.size() || !s.fds_to_close.empty();
  }

  auto generate(state_type s, std::size_t max_ops) const -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;
    ops.reserve(max_ops);

    // prioritize closing files to free up fds
    while (ops.size() < max_ops && !s.fds_to_close.empty()) {
      int fd = s.fds_to_close.back();
      s.fds_to_close.pop_back();
      // use user_data high bit to mark close operations
      ops.push_back(evring::operation::make_close(evring::handle::invalid(),
                                                  (1ULL << 63) | static_cast<std::uint64_t>(fd)));
      // Note: we need to handle close differently - it needs a handle, not raw fd
      // For now, let's simplify and just track the pattern
    }

    // then open new files
    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      ops.push_back(evring::operation::make_open(
          paths[s.next_to_submit], O_CREAT | O_WRONLY | O_TRUNC, mode, s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  auto step(state_type s, evring::event const& e) const -> evring::step_result<state_type> {
    // check if this is a close completion (high bit set in user_data)
    if (e.user_data & (1ULL << 63)) {
      s.closed++;
      return {std::move(s), {}};
    }

    // otherwise it's an open completion
    s.opened++;
    if (e.result >= 0) {
      s.succeeded++;
      s.fds_to_close.push_back(e.result);
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  auto done(const state_type& s) const -> bool {
    return s.opened >= paths.size() && s.closed >= s.succeeded;
  }
};

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

  // Test with generator machine
  auto ring = evring::make_io_uring_ring(256);
  bulk_stat_machine machine{std::span{paths.data(), paths.size()},
                            std::span{buffers.data(), buffers.size()}};

  double elapsed = measure([&] {
    auto final_state = evring::run_generate(machine, *ring);
    assert(final_state.completed == paths.size());
    assert(final_state.failed == 0);
  });

  double ops_per_sec = static_cast<double>(paths.size()) / elapsed;
  std::printf("test_bulk_stat_generator: %zu files in %.3fs (%.0f ops/s)\n", paths.size(), elapsed,
              ops_per_sec);

  // Compare with current bulk API
  auto ring2 = evring::make_io_uring_ring(256);
  std::vector<struct statx> buffers2(paths.size());

  double elapsed_bulk = measure([&] {
    evring::bulk_stat(*ring2, std::span{paths.data(), paths.size()},
                      std::span{buffers2.data(), buffers2.size()});
  });

  double ops_per_sec_bulk = static_cast<double>(paths.size()) / elapsed_bulk;
  std::printf("bulk_stat (current):       %zu files in %.3fs (%.0f ops/s)\n", paths.size(),
              elapsed_bulk, ops_per_sec_bulk);

  double ratio = ops_per_sec / ops_per_sec_bulk;
  std::printf("generator/bulk ratio: %.2fx\n", ratio);

  // Should be within 20% of bulk performance
  assert(ratio > 0.8);

  std::printf("test_bulk_stat_generator: PASSED\n\n");
}

void test_bulk_stat_replay() {
  std::printf("test_bulk_stat_replay: testing replay...\n");

  // Small set for replay test
  std::vector<const char*> paths = {"/etc/hostname", "/etc/passwd", "/etc/group"};
  std::vector<struct statx> buffers(paths.size());

  auto ring = evring::make_io_uring_ring(256);
  bulk_stat_machine machine{std::span{paths.data(), paths.size()},
                            std::span{buffers.data(), buffers.size()}};

  // Run with tracing
  auto [final_state, trace] = evring::run_generate_traced(machine, *ring);

  std::printf("test_bulk_stat_replay: captured %zu events\n", trace.size());
  assert(final_state.completed == paths.size());
  assert(final_state.succeeded == paths.size());

  // Replay without I/O
  std::vector<struct statx> buffers2(paths.size());
  bulk_stat_machine machine2{std::span{paths.data(), paths.size()},
                             std::span{buffers2.data(), buffers2.size()}};

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

} // namespace

int main() {
  test_posix_baseline();
  test_bulk_stat_generator();
  test_bulk_stat_replay();

  std::printf("all generator tests passed!\n");
  return 0;
}
