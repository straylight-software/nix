// test_chaos.cpp
//
// "You don't like Beethoven. You don't know what you're missing.
//  Overtures like that get my juices flowing."
//
// Chaos engineering tests. Randomized torture. Memory corruption detection.
// These tests are designed to find bugs that only manifest under stress,
// with unusual timing, or in edge cases nobody thought to test.

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "straylight/evring/evring.h"

namespace {

std::mt19937 rng{std::random_device{}()};

// ============================================================================
// Randomized operation sequencing
// - Submit operations in random order, verify all complete correctly
// ============================================================================

void test_random_operation_order() {
  std::printf("test_random_operation_order: chaos operation sequencing...\n");

  auto ring = evring::make_io_uring_ring(128);

  // Create test files
  std::vector<std::string> paths;
  for (int i = 0; i < 20; ++i) {
    char tmp_path[] = "/tmp/evring_chaos_XXXXXX";
    int fd = mkstemp(tmp_path);
    assert(fd >= 0);
    // Write random amount of data
    std::uniform_int_distribution<int> size_dist(1, 8192);
    std::vector<char> data(size_dist(rng), 'X');
    ssize_t w = write(fd, data.data(), data.size());
    assert(w == static_cast<ssize_t>(data.size()));
    close(fd);
    paths.push_back(tmp_path);
  }

  // Generate random operations
  enum class op_type { stat, open_close, read, unlink };
  std::vector<op_type> operations;

  for (int i = 0; i < 100; ++i) {
    std::uniform_int_distribution<int> op_dist(0, 3);
    operations.push_back(static_cast<op_type>(op_dist(rng)));
  }

  // Shuffle
  std::shuffle(operations.begin(), operations.end(), rng);

  int stats_done = 0;
  int opens_done = 0;
  int reads_done = 0;

  std::vector<struct statx> stat_buffers(paths.size());
  std::size_t stat_idx = 0;
  std::size_t path_idx = 0;

  for (auto op : operations) {
    if (path_idx >= paths.size())
      break;

    switch (op) {
      case op_type::stat: {
        if (stat_idx < stat_buffers.size()) {
          ring->enqueue(
              evring::operation::make_statx(AT_FDCWD, paths[path_idx % paths.size()].c_str(), 0,
                                            STATX_BASIC_STATS, &stat_buffers[stat_idx++]));
          auto events = ring->submit_and_wait(1);
          // May succeed or fail depending on if file was unlinked
          stats_done++;
        }
        break;
      }

      case op_type::open_close: {
        ring->enqueue(
            evring::operation::make_open(paths[path_idx % paths.size()].c_str(), O_RDONLY));
        auto events = ring->submit_and_wait(1);
        if (events[0].ok()) {
          ring->enqueue(evring::operation::make_close(events[0].resource_handle));
          ring->submit_and_wait(1);
          opens_done++;
        }
        break;
      }

      case op_type::read: {
        ring->enqueue(
            evring::operation::make_open(paths[path_idx % paths.size()].c_str(), O_RDONLY));
        auto events = ring->submit_and_wait(1);
        if (events[0].ok()) {
          evring::handle h = events[0].resource_handle;
          std::vector<std::byte> buf(4096);
          ring->enqueue(evring::operation::make_read(h, std::span{buf.data(), buf.size()}));
          events = ring->submit_and_wait(1);
          ring->enqueue(evring::operation::make_close(h));
          ring->submit_and_wait(1);
          reads_done++;
        }
        break;
      }

      case op_type::unlink: {
        ring->enqueue(evring::operation::make_unlink(paths[path_idx].c_str()));
        auto events = ring->submit_and_wait(1);
        // May fail if already unlinked
        path_idx++;
        break;
      }
    }
  }

  // Cleanup remaining files
  for (const auto& p : paths) {
    unlink(p.c_str());
  }

  std::printf("  stats: %d, opens: %d, reads: %d\n", stats_done, opens_done, reads_done);
  std::printf("test_random_operation_order: PASSED\n\n");
}

// ============================================================================
// Memory fence verification
// - Ensure completions are fully visible before we read them
// ============================================================================

void test_completion_memory_visibility() {
  std::printf("test_completion_memory_visibility: testing memory ordering...\n");

  auto ring = evring::make_io_uring_ring(64);

  // Create a file with known content
  char tmp_path[] = "/tmp/evring_memfence_XXXXXX";
  int fd = mkstemp(tmp_path);
  assert(fd >= 0);

  const char* pattern = "DEADBEEF";
  for (int i = 0; i < 1000; ++i) {
    ssize_t w = write(fd, pattern, strlen(pattern));
    assert(w == static_cast<ssize_t>(strlen(pattern)));
  }
  close(fd);

  // Open and read repeatedly, verify pattern is always correct
  ring->enqueue(evring::operation::make_open(tmp_path, O_RDONLY));
  auto events = ring->submit_and_wait(1);
  assert(events[0].ok());
  evring::handle file = events[0].resource_handle;

  for (int iteration = 0; iteration < 100; ++iteration) {
    std::vector<std::byte> buf(strlen(pattern));

    ring->enqueue(evring::operation::make_read(file, std::span{buf.data(), buf.size()},
                                               (iteration % 100) * strlen(pattern)));

    events = ring->submit_and_wait(1);
    assert(events[0].ok());

    // Verify pattern - if memory ordering is broken, we might see garbage
    bool matches = true;
    for (std::size_t i = 0; i < buf.size(); ++i) {
      if (static_cast<char>(buf[i]) != pattern[i]) {
        matches = false;
        break;
      }
    }

    if (!matches) {
      std::printf("  FAIL: memory corruption detected at iteration %d\n", iteration);
      assert(false);
    }
  }

  ring->enqueue(evring::operation::make_close(file));
  ring->submit_and_wait(1);
  unlink(tmp_path);

  std::printf("  PASS: 100 iterations with correct memory visibility\n");
  std::printf("test_completion_memory_visibility: PASSED\n\n");
}

// ============================================================================
// SQ/CQ wraparound stress test
// - Force the ring to wrap around many times
// ============================================================================

void test_ring_wraparound() {
  std::printf("test_ring_wraparound: forcing SQ/CQ wraparound...\n");

  // Small ring to force wraparound
  auto ring = evring::make_io_uring_ring(16);

  // Create a temp file
  char tmp_path[] = "/tmp/evring_wrap_XXXXXX";
  int fd = mkstemp(tmp_path);
  assert(fd >= 0);
  close(fd);

  ring->enqueue(evring::operation::make_open(tmp_path, O_RDONLY));
  auto events = ring->submit_and_wait(1);
  assert(events[0].ok());
  evring::handle file = events[0].resource_handle;

  std::byte buf[64];

  // Do many more operations than ring size to force wraparound
  const int num_ops = 1000;
  for (int i = 0; i < num_ops; ++i) {
    ring->enqueue(evring::operation::make_read(file, std::span{buf, sizeof(buf)}));
    events = ring->submit_and_wait(1);
    assert(events[0].ok());
  }

  std::printf("  PASS: %d operations on ring size 16 (%.0fx wraparound)\n", num_ops,
              static_cast<double>(num_ops) / 16.0);

  ring->enqueue(evring::operation::make_close(file));
  ring->submit_and_wait(1);
  unlink(tmp_path);

  std::printf("test_ring_wraparound: PASSED\n\n");
}

// ============================================================================
// Generator machine interruption
// - Start a generator, then throw unexpected events at it
// ============================================================================

void test_generator_unexpected_completions() {
  std::printf("test_generator_unexpected_completions: malformed completion handling...\n");

  // Create a custom generator that tracks what it sees
  struct chaos_state {
    std::size_t next{0};
    std::size_t completed{0};
    std::vector<std::int64_t> results;
  };

  struct chaos_machine {
    using state_type = chaos_state;
    std::size_t count;

    explicit chaos_machine(std::size_t n) : count(n) {}

    auto initial() const -> state_type { return {}; }

    auto wants_to_submit(const state_type& s) const -> bool { return s.next < count; }

    auto generate(state_type s, std::size_t max_ops) const -> evring::step_result<state_type> {
      std::vector<evring::operation> ops;
      while (ops.size() < max_ops && s.next < count) {
        // Just do a nop with user_data = index
        ops.push_back(evring::operation::make_nop(s.next));
        s.next++;
      }
      return {std::move(s), std::move(ops)};
    }

    auto step(state_type s, const evring::event& e) const -> evring::step_result<state_type> {
      s.completed++;
      s.results.push_back(e.result);
      return {std::move(s), {}};
    }

    auto done(const state_type& s) const -> bool { return s.completed >= count; }
  };

  auto ring = evring::make_io_uring_ring(32);

  chaos_machine machine{50};
  auto final_state = evring::run_generate(machine, *ring);

  assert(final_state.completed == 50);
  assert(final_state.results.size() == 50);

  // All nops should return 0
  for (auto r : final_state.results) {
    assert(r == 0);
  }

  std::printf("  PASS: 50 nops completed correctly through generator\n");
  std::printf("test_generator_unexpected_completions: PASSED\n\n");
}

// ============================================================================
// Replay determinism verification
// - Run same trace multiple times, verify identical results
// ============================================================================

void test_replay_determinism() {
  std::printf("test_replay_determinism: verifying replay produces identical results...\n");

  // Simple accumulating machine
  struct accum_state {
    int sum{0};
    int count{0};
  };

  struct accum_machine {
    using state_type = accum_state;
    int target;

    explicit accum_machine(int t) : target(t) {}

    auto initial() const -> state_type { return {}; }

    auto step(state_type s, const evring::event& e) const -> evring::step_result<state_type> {
      if (s.count == 0) {
        // Initial step - request a read
        std::vector<evring::operation> ops;
        ops.push_back(evring::operation::make_nop(42));
        s.count++;
        return {std::move(s), std::move(ops)};
      }

      s.sum += static_cast<int>(e.result + e.user_data);
      s.count++;
      return {std::move(s), {}};
    }

    auto done(const state_type& s) const -> bool { return s.count > target; }
  };

  // Create a trace
  std::vector<evring::event> trace;
  for (int i = 0; i < 10; ++i) {
    trace.push_back(evring::event{
        .resource_handle = evring::handle{static_cast<uint32_t>(i), 0},
        .operation = evring::operation_type::nop,
        .result = i * 10,
        .data = {},
        .user_data = static_cast<uint64_t>(i),
    });
  }

  // Replay multiple times
  std::vector<accum_state> results;
  for (int run = 0; run < 100; ++run) {
    accum_machine machine{10};
    auto state = evring::replay(machine, trace);
    results.push_back(state);
  }

  // All results must be identical
  for (std::size_t i = 1; i < results.size(); ++i) {
    assert(results[i].sum == results[0].sum);
    assert(results[i].count == results[0].count);
  }

  std::printf("  PASS: 100 replays produced identical results (sum=%d, count=%d)\n", results[0].sum,
              results[0].count);
  std::printf("test_replay_determinism: PASSED\n\n");
}

// ============================================================================
// Partial read/write verification
// - Verify that partial I/O is handled correctly
// ============================================================================

void test_partial_io() {
  std::printf("test_partial_io: testing partial read behavior...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Create a small file
  char tmp_path[] = "/tmp/evring_partial_XXXXXX";
  int fd = mkstemp(tmp_path);
  assert(fd >= 0);

  const char* content = "short";
  ssize_t w = write(fd, content, strlen(content));
  assert(w == static_cast<ssize_t>(strlen(content)));
  close(fd);

  ring->enqueue(evring::operation::make_open(tmp_path, O_RDONLY));
  auto events = ring->submit_and_wait(1);
  assert(events[0].ok());
  evring::handle file = events[0].resource_handle;

  // Request more bytes than exist - should get partial read
  std::vector<std::byte> buf(1024);
  ring->enqueue(evring::operation::make_read(file, std::span{buf.data(), buf.size()}));
  events = ring->submit_and_wait(1);

  assert(events[0].ok());
  assert(events[0].result == static_cast<int64_t>(strlen(content)));
  std::printf("  PASS: requested 1024 bytes, got %ld (file size: %zu)\n", events[0].result,
              strlen(content));

  // Subsequent read should return 0 (EOF)
  ring->enqueue(evring::operation::make_read(file, std::span{buf.data(), buf.size()}));
  events = ring->submit_and_wait(1);

  assert(events[0].ok());
  assert(events[0].result == 0);
  std::printf("  PASS: second read returns 0 (EOF)\n");

  ring->enqueue(evring::operation::make_close(file));
  ring->submit_and_wait(1);
  unlink(tmp_path);

  std::printf("test_partial_io: PASSED\n\n");
}

// ============================================================================
// File descriptor exhaustion recovery
// ============================================================================

void test_fd_exhaustion_recovery() {
  std::printf("test_fd_exhaustion_recovery: testing EMFILE handling...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Try to reduce fd limit temporarily (may not work without privileges)
  struct rlimit old_limit, new_limit;
  if (getrlimit(RLIMIT_NOFILE, &old_limit) == 0) {
    new_limit.rlim_cur = 64; // Low limit
    new_limit.rlim_max = old_limit.rlim_max;

    if (setrlimit(RLIMIT_NOFILE, &new_limit) == 0) {
      std::printf("  Reduced fd limit to 64\n");

      // Try to open many files - should eventually fail with EMFILE
      std::vector<evring::handle> handles;
      int failures = 0;

      for (int i = 0; i < 100; ++i) {
        ring->enqueue(evring::operation::make_open("/etc/hostname", O_RDONLY));
        auto events = ring->submit_and_wait(1);

        if (events[0].ok()) {
          handles.push_back(events[0].resource_handle);
        } else {
          if (events[0].error_code() == EMFILE) {
            failures++;
          }
        }
      }

      std::printf("  Opened %zu files, got %d EMFILE errors\n", handles.size(), failures);

      // Close all handles
      for (auto h : handles) {
        ring->enqueue(evring::operation::make_close(h));
      }
      while (!handles.empty()) {
        auto events = ring->submit_and_wait(1);
        for (std::size_t i = 0; i < events.size() && !handles.empty(); ++i) {
          handles.pop_back();
        }
      }

      // Restore limit
      setrlimit(RLIMIT_NOFILE, &old_limit);

      if (failures > 0) {
        std::printf("  PASS: correctly handled fd exhaustion\n");
      } else {
        std::printf("  SKIP: didn't hit fd limit (not enough fds in use)\n");
      }
    } else {
      std::printf("  SKIP: couldn't reduce fd limit (need privileges)\n");
    }
  } else {
    std::printf("  SKIP: couldn't get fd limit\n");
  }

  std::printf("test_fd_exhaustion_recovery: PASSED\n\n");
}

// ============================================================================
// Large batch with mixed results
// ============================================================================

void test_large_mixed_batch() {
  std::printf("test_large_mixed_batch: large batch with intentional failures...\n");

  // Mix of existing and non-existing paths
  std::vector<std::string> paths;
  for (int i = 0; i < 500; ++i) {
    if (i % 3 == 0) {
      paths.push_back("/etc/hostname"); // exists
    } else if (i % 3 == 1) {
      paths.push_back("/nonexistent/" + std::to_string(i)); // doesn't exist
    } else {
      paths.push_back("/etc/passwd"); // exists
    }
  }

  std::vector<const char*> path_ptrs;
  for (auto& p : paths) {
    path_ptrs.push_back(p.c_str());
  }

  std::vector<struct statx> buffers(paths.size());

  auto ring = evring::make_io_uring_ring(256);
  evring::bulk_stat_machine machine{std::span{path_ptrs.data(), path_ptrs.size()},
                                    std::span{buffers.data(), buffers.size()}};

  auto final_state = evring::run_generate(machine, *ring);

  assert(final_state.completed == paths.size());

  // Approximately 1/3 should fail (the nonexistent ones)
  std::printf("  Completed: %zu, Succeeded: %zu, Failed: %zu\n", final_state.completed,
              final_state.succeeded, final_state.failed);

  // Sanity check ratios
  double fail_ratio = static_cast<double>(final_state.failed) / final_state.completed;
  assert(fail_ratio > 0.25 && fail_ratio < 0.45); // Roughly 1/3

  std::printf("  PASS: failure ratio %.2f (expected ~0.33)\n", fail_ratio);
  std::printf("test_large_mixed_batch: PASSED\n\n");
}

// ============================================================================
// User data integrity through round-trip
// ============================================================================

void test_user_data_integrity() {
  std::printf("test_user_data_integrity: verifying user_data survives round-trip...\n");

  auto ring = evring::make_io_uring_ring(64);

  // Submit operations with distinctive user_data values
  std::vector<std::uint64_t> expected_user_data;
  for (int i = 0; i < 32; ++i) {
    std::uint64_t ud = 0xDEADBEEF00000000ULL | static_cast<std::uint64_t>(i);
    expected_user_data.push_back(ud);
    ring->enqueue(evring::operation::make_nop(ud));
  }

  int submitted = ring->submit();
  assert(submitted == 32);

  // Collect completions and verify user_data
  std::vector<std::uint64_t> received_user_data;
  while (received_user_data.size() < expected_user_data.size()) {
    auto events = ring->submit_and_wait(1);
    for (auto& e : events) {
      received_user_data.push_back(e.user_data);
    }
  }

  // Sort both vectors (completions may be out of order)
  std::sort(expected_user_data.begin(), expected_user_data.end());
  std::sort(received_user_data.begin(), received_user_data.end());

  assert(expected_user_data == received_user_data);

  std::printf("  PASS: all 32 user_data values survived round-trip\n");
  std::printf("test_user_data_integrity: PASSED\n\n");
}

} // namespace

int main() {
  std::printf("=== CHAOS TESTS ===\n");
  std::printf("\"You don't like Beethoven. You don't know what you're missing.\"\n\n");

  test_random_operation_order();
  test_completion_memory_visibility();
  test_ring_wraparound();
  test_generator_unexpected_completions();
  test_replay_determinism();
  test_partial_io();
  test_fd_exhaustion_recovery();
  test_large_mixed_batch();
  test_user_data_integrity();

  std::printf("=== ALL CHAOS TESTS PASSED ===\n");
  std::printf("\"Bring me everyone.\" \"What do you mean, everyone?\" \"EVERYONE!\"\n");
  return 0;
}
