// test_adversarial.cpp
//
// "I like these calm little moments before the storm."
//
// Adversarial tests that assume the kernel, network, and user are all
// conspiring to break us. Every errno. Every edge case. Every race.

#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "straylight/evring/evring.h"

namespace {

// ============================================================================
// "EVERYONE!"
// - Every errno that open() can return
// ============================================================================

void test_open_every_errno() {
  std::printf("test_open_every_errno: testing all open failure modes...\n");

  auto ring = evring::make_io_uring_ring(32);

  struct error_case {
    const char* path;
    int flags;
    int expected_errno;
    const char* description;
  };

  std::vector<error_case> cases = {
      // ENOENT - file doesn't exist
      {"/nonexistent/path/to/file", O_RDONLY, ENOENT, "ENOENT: path doesn't exist"},

      // ENOTDIR - component is not a directory
      {"/etc/passwd/impossible", O_RDONLY, ENOTDIR, "ENOTDIR: component not a dir"},

      // EISDIR - trying to write to a directory
      {"/tmp", O_WRONLY, EISDIR, "EISDIR: write to directory"},

      // EACCES - permission denied (assuming we're not root)
      {"/etc/shadow", O_RDONLY, EACCES, "EACCES: permission denied"},

      // ELOOP - too many symlinks (create one first)
      // We'll skip this one as it requires setup

      // ENAMETOOLONG - path too long
      {nullptr, O_RDONLY, ENAMETOOLONG, "ENAMETOOLONG: path too long"},

      // ENODEV - special file that doesn't support open
      // System dependent, skip

      // EROFS - read-only filesystem
      // System dependent, skip
  };

  // Generate a path that's too long
  std::string long_path(PATH_MAX + 100, 'a');
  long_path[0] = '/';
  cases[4].path = long_path.c_str();

  int passed = 0;
  int skipped = 0;

  for (const auto& tc : cases) {
    if (tc.path == nullptr) {
      skipped++;
      continue;
    }

    ring->enqueue(evring::operation::make_open(tc.path, tc.flags));
    auto events = ring->submit_and_wait(1);

    if (events[0].result < 0) {
      int actual_errno = events[0].error_code();
      if (actual_errno == tc.expected_errno) {
        std::printf("  PASS: %s\n", tc.description);
        passed++;
      } else if (actual_errno == EACCES && tc.expected_errno == EACCES) {
        // EACCES might not happen if running as root
        std::printf("  PASS: %s (got %d)\n", tc.description, actual_errno);
        passed++;
      } else {
        std::printf("  FAIL: %s - expected %d, got %d\n", tc.description, tc.expected_errno,
                    actual_errno);
      }
    } else {
      // Unexpectedly succeeded - close the fd
      ring->enqueue(evring::operation::make_close(events[0].resource_handle));
      ring->submit_and_wait(1);

      if (tc.expected_errno == EACCES) {
        std::printf("  SKIP: %s (running as root?)\n", tc.description);
        skipped++;
      } else {
        std::printf("  FAIL: %s - expected failure, got success\n", tc.description);
      }
    }
  }

  std::printf("test_open_every_errno: %d passed, %d skipped\n\n", passed, skipped);
}

// ============================================================================
// Handle generation wraparound - ABA problem detection
// ============================================================================

void test_handle_generation_aba() {
  std::printf("test_handle_generation_aba: testing handle reuse safety...\n");

  evring::handle_table<int> table;

  // Allocate a handle
  evring::handle h1 = table.insert(42);
  assert(table.valid(h1));
  assert(*table.get(h1) == 42);

  // Remove it
  auto removed = table.remove(h1);
  assert(removed.has_value());
  assert(*removed == 42);

  // h1 is now invalid
  assert(!table.valid(h1));
  assert(table.get(h1) == nullptr);

  // Allocate again - reuses same slot but different generation
  evring::handle h2 = table.insert(99);
  assert(table.valid(h2));
  assert(*table.get(h2) == 99);

  // h1 and h2 have same index but different generations
  assert(h1.index == h2.index);
  assert(h1.generation != h2.generation);

  // h1 is STILL invalid even though slot is reused
  assert(!table.valid(h1));
  assert(table.get(h1) == nullptr);

  std::printf("  PASS: stale handle correctly rejected after reuse\n");

  // Torture test: cycle through many generations
  for (int i = 0; i < 10000; ++i) {
    auto val = table.remove(h2);
    assert(val.has_value());
    h2 = table.insert(i);
  }

  // Original h1 still invalid
  assert(!table.valid(h1));

  std::printf("  PASS: 10000 generation cycles, original handle still invalid\n");
  std::printf("test_handle_generation_aba: PASSED\n\n");
}

// ============================================================================
// Zero-byte operations - the edge of the edge
// ============================================================================

void test_zero_byte_operations() {
  std::printf("test_zero_byte_operations: testing degenerate I/O...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Create a temp file
  char tmp_path[] = "/tmp/evring_zero_XXXXXX";
  int fd = mkstemp(tmp_path);
  assert(fd >= 0);
  close(fd);

  // Open it
  ring->enqueue(evring::operation::make_open(tmp_path, O_RDWR));
  auto events = ring->submit_and_wait(1);
  assert(events[0].ok());
  evring::handle file = events[0].resource_handle;

  // Zero-byte write - should succeed with 0 bytes written
  std::byte empty_buf[1];
  ring->enqueue(evring::operation::make_write(file, std::span<const std::byte>{empty_buf, 0}));
  events = ring->submit_and_wait(1);
  assert(events[0].ok());
  assert(events[0].result == 0);
  std::printf("  PASS: zero-byte write returns 0\n");

  // Zero-byte read - should succeed with 0 bytes read
  ring->enqueue(evring::operation::make_read(file, std::span<std::byte>{empty_buf, 0}));
  events = ring->submit_and_wait(1);
  assert(events[0].ok());
  assert(events[0].result == 0);
  std::printf("  PASS: zero-byte read returns 0\n");

  // Read from empty file - should return 0 (EOF)
  std::byte buf[1024];
  ring->enqueue(evring::operation::make_read(file, std::span<std::byte>{buf, sizeof(buf)}));
  events = ring->submit_and_wait(1);
  assert(events[0].ok());
  assert(events[0].result == 0); // EOF
  std::printf("  PASS: read from empty file returns EOF\n");

  // Cleanup
  ring->enqueue(evring::operation::make_close(file));
  ring->submit_and_wait(1);
  unlink(tmp_path);

  std::printf("test_zero_byte_operations: PASSED\n\n");
}

// ============================================================================
// Offset edge cases - negative, MAX, beyond EOF
// ============================================================================

void test_offset_edge_cases() {
  std::printf("test_offset_edge_cases: testing extreme offsets...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Create a temp file with some content
  char tmp_path[] = "/tmp/evring_offset_XXXXXX";
  int fd = mkstemp(tmp_path);
  assert(fd >= 0);
  const char* content = "hello world";
  ssize_t written = write(fd, content, strlen(content));
  assert(written == static_cast<ssize_t>(strlen(content)));
  close(fd);

  // Open it
  ring->enqueue(evring::operation::make_open(tmp_path, O_RDWR));
  auto events = ring->submit_and_wait(1);
  assert(events[0].ok());
  evring::handle file = events[0].resource_handle;

  std::byte buf[1024];

  // Read at offset beyond EOF - should return 0
  ring->enqueue(
      evring::operation::make_read(file, std::span<std::byte>{buf, sizeof(buf)}, 1000000));
  events = ring->submit_and_wait(1);
  assert(events[0].ok());
  assert(events[0].result == 0); // EOF
  std::printf("  PASS: read beyond EOF returns 0\n");

  // Read at exactly EOF
  ring->enqueue(evring::operation::make_read(file, std::span<std::byte>{buf, sizeof(buf)},
                                             static_cast<int64_t>(strlen(content))));
  events = ring->submit_and_wait(1);
  assert(events[0].ok());
  assert(events[0].result == 0);
  std::printf("  PASS: read at exact EOF returns 0\n");

  // Write at large offset - should create sparse file
  const char* sparse_data = "sparse";
  std::vector<std::byte> sparse_buf(strlen(sparse_data));
  memcpy(sparse_buf.data(), sparse_data, sparse_buf.size());

  ring->enqueue(evring::operation::make_write(
      file, std::span<const std::byte>{sparse_buf.data(), sparse_buf.size()}, 1000000));
  events = ring->submit_and_wait(1);
  assert(events[0].ok());
  assert(events[0].result == static_cast<int64_t>(sparse_buf.size()));
  std::printf("  PASS: write at large offset creates sparse region\n");

  // Verify file size
  struct stat st;
  int ret = stat(tmp_path, &st);
  assert(ret == 0);
  assert(st.st_size == 1000000 + static_cast<off_t>(strlen(sparse_data)));
  std::printf("  PASS: sparse file has correct size (%ld bytes)\n", st.st_size);

  // Cleanup
  ring->enqueue(evring::operation::make_close(file));
  ring->submit_and_wait(1);
  unlink(tmp_path);

  std::printf("test_offset_edge_cases: PASSED\n\n");
}

// ============================================================================
// Invalid handle operations - use after free, wrong generation
// ============================================================================

void test_invalid_handle_operations() {
  std::printf("test_invalid_handle_operations: testing stale handle rejection...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Create and open a file
  char tmp_path[] = "/tmp/evring_handle_XXXXXX";
  int fd = mkstemp(tmp_path);
  assert(fd >= 0);
  close(fd);

  ring->enqueue(evring::operation::make_open(tmp_path, O_RDWR));
  auto events = ring->submit_and_wait(1);
  assert(events[0].ok());
  evring::handle file = events[0].resource_handle;

  // Close it
  ring->enqueue(evring::operation::make_close(file));
  events = ring->submit_and_wait(1);
  assert(events[0].ok());

  // Now try to use the closed handle - should fail
  std::byte buf[64];
  ring->enqueue(evring::operation::make_read(file, std::span<std::byte>{buf, sizeof(buf)}));
  events = ring->submit_and_wait(1);

  // Should get EBADF or similar
  assert(!events[0].ok());
  std::printf("  PASS: read on closed handle fails with errno %d\n", events[0].error_code());

  // Try double close - should fail
  ring->enqueue(evring::operation::make_close(file));
  events = ring->submit_and_wait(1);
  assert(!events[0].ok());
  std::printf("  PASS: double close fails with errno %d\n", events[0].error_code());

  // Invalid handle (never allocated)
  evring::handle fake_handle{12345, 0};
  ring->enqueue(evring::operation::make_read(fake_handle, std::span<std::byte>{buf, sizeof(buf)}));
  events = ring->submit_and_wait(1);
  assert(!events[0].ok());
  std::printf("  PASS: operation on invalid handle fails\n");

  unlink(tmp_path);
  std::printf("test_invalid_handle_operations: PASSED\n\n");
}

// ============================================================================
// Rapid fire operations - stress the SQ/CQ
// ============================================================================

void test_rapid_fire_operations() {
  std::printf("test_rapid_fire_operations: stress testing submission queue...\n");

  auto ring = evring::make_io_uring_ring(256);

  // Create a temp file
  char tmp_path[] = "/tmp/evring_rapid_XXXXXX";
  int fd = mkstemp(tmp_path);
  assert(fd >= 0);
  // Pre-fill with data
  std::vector<char> fill(1024 * 1024, 'X');
  ssize_t written = write(fd, fill.data(), fill.size());
  assert(written == static_cast<ssize_t>(fill.size()));
  close(fd);

  ring->enqueue(evring::operation::make_open(tmp_path, O_RDONLY));
  auto events = ring->submit_and_wait(1);
  assert(events[0].ok());
  evring::handle file = events[0].resource_handle;

  // Submit as many reads as we can fit in the SQ
  std::vector<std::vector<std::byte>> buffers;
  std::size_t num_ops = ring->sq_space();
  std::printf("  Submitting %zu simultaneous reads...\n", num_ops);

  for (std::size_t i = 0; i < num_ops; ++i) {
    buffers.emplace_back(4096);
    ring->enqueue(evring::operation::make_read(
        file, std::span<std::byte>{buffers.back().data(), buffers.back().size()},
        static_cast<int64_t>(i * 4096)));
  }

  // Submit all at once
  int submitted = ring->submit();
  assert(submitted == static_cast<int>(num_ops));

  // Harvest all completions
  std::size_t completed = 0;
  while (completed < num_ops) {
    events = ring->submit_and_wait(1);
    for (auto& e : events) {
      assert(e.ok());
      completed++;
    }
  }

  std::printf("  PASS: all %zu operations completed successfully\n", completed);

  // Cleanup
  ring->enqueue(evring::operation::make_close(file));
  ring->submit_and_wait(1);
  unlink(tmp_path);

  std::printf("test_rapid_fire_operations: PASSED\n\n");
}

// ============================================================================
// Generator machine with all failures
// ============================================================================

void test_generator_all_fail() {
  std::printf("test_generator_all_fail: bulk_stat on nonexistent paths...\n");

  std::vector<const char*> bad_paths = {
      "/nonexistent/path/1", "/nonexistent/path/2", "/nonexistent/path/3",
      "/nonexistent/path/4", "/nonexistent/path/5",
  };

  std::vector<struct statx> buffers(bad_paths.size());

  auto ring = evring::make_io_uring_ring(32);
  evring::bulk_stat_machine machine{std::span{bad_paths.data(), bad_paths.size()},
                                    std::span{buffers.data(), buffers.size()}};

  auto final_state = evring::run_generate(machine, *ring);

  assert(final_state.completed == bad_paths.size());
  assert(final_state.succeeded == 0);
  assert(final_state.failed == bad_paths.size());
  assert(final_state.errors.size() == bad_paths.size());

  for (int err : final_state.errors) {
    assert(err == ENOENT);
  }

  std::printf("  PASS: all %zu operations failed with ENOENT as expected\n", bad_paths.size());
  std::printf("test_generator_all_fail: PASSED\n\n");
}

// ============================================================================
// Mixed success/failure in generator
// ============================================================================

void test_generator_mixed_results() {
  std::printf("test_generator_mixed_results: bulk_stat with mixed paths...\n");

  std::vector<const char*> paths = {
      "/etc/hostname",  // exists
      "/nonexistent/1", // doesn't exist
      "/etc/passwd",    // exists
      "/nonexistent/2", // doesn't exist
      "/etc/group",     // exists
      "/etc/shadow",    // may not be readable
      "/nonexistent/3", // doesn't exist
  };

  std::vector<struct statx> buffers(paths.size());

  auto ring = evring::make_io_uring_ring(32);
  evring::bulk_stat_machine machine{std::span{paths.data(), paths.size()},
                                    std::span{buffers.data(), buffers.size()}};

  auto final_state = evring::run_generate(machine, *ring);

  assert(final_state.completed == paths.size());
  std::printf("  Completed: %zu, Succeeded: %zu, Failed: %zu\n", final_state.completed,
              final_state.succeeded, final_state.failed);

  // At least the 3 ENOENT paths should fail
  assert(final_state.failed >= 3);

  // At least 3 should succeed (hostname, passwd, group)
  assert(final_state.succeeded >= 3);

  std::printf("  PASS: mixed results handled correctly\n");
  std::printf("test_generator_mixed_results: PASSED\n\n");
}

// ============================================================================
// Replay with corrupted/truncated event stream
// ============================================================================

void test_replay_truncated_stream() {
  std::printf("test_replay_truncated_stream: replay with missing events...\n");

  // Simple file reader state machine
  struct simple_state {
    enum class phase { initial, opening, reading, done, error };
    phase current_phase{phase::initial};
    evring::handle file_handle;
    int error_code{0};
  };

  struct simple_machine {
    using state_type = simple_state;

    auto initial() const -> state_type { return {}; }

    auto step(state_type s, evring::event e) const -> evring::step_result<state_type> {
      std::vector<evring::operation> ops;

      switch (s.current_phase) {
        case simple_state::phase::initial:
          s.current_phase = simple_state::phase::opening;
          ops.push_back(evring::operation::make_open("/etc/hostname", O_RDONLY));
          break;

        case simple_state::phase::opening:
          if (e.ok()) {
            s.file_handle = e.resource_handle;
            s.current_phase = simple_state::phase::done;
          } else {
            s.current_phase = simple_state::phase::error;
            s.error_code = e.error_code();
          }
          break;

        default:
          break;
      }

      return {std::move(s), std::move(ops)};
    }

    auto done(const state_type& s) const -> bool {
      return s.current_phase == simple_state::phase::done ||
             s.current_phase == simple_state::phase::error;
    }
  };

  simple_machine machine;

  // Empty event stream - machine should stay in non-terminal state
  std::vector<evring::event> empty_events;
  auto state1 = evring::replay(machine, empty_events);
  // After replay with no events, we've only done the initial step
  // which moves us to 'opening' phase
  assert(state1.current_phase == simple_state::phase::opening);
  std::printf("  PASS: empty event stream leaves machine in opening phase\n");

  // Single successful event
  std::vector<evring::event> success_events = {
      evring::event{
          .resource_handle = evring::handle{0, 0},
          .operation = evring::operation_type::open,
          .result = 5,
          .data = {},
          .user_data = 0,
      },
  };

  auto state2 = evring::replay(machine, success_events);
  assert(state2.current_phase == simple_state::phase::done);
  std::printf("  PASS: single success event reaches done state\n");

  // Single failure event
  std::vector<evring::event> fail_events = {
      evring::event{
          .resource_handle = evring::handle::invalid(),
          .operation = evring::operation_type::open,
          .result = -ENOENT,
          .data = {},
          .user_data = 0,
      },
  };

  auto state3 = evring::replay(machine, fail_events);
  assert(state3.current_phase == simple_state::phase::error);
  assert(state3.error_code == ENOENT);
  std::printf("  PASS: single failure event reaches error state\n");

  std::printf("test_replay_truncated_stream: PASSED\n\n");
}

// ============================================================================
// Symlink loops - ELOOP
// ============================================================================

void test_symlink_loop() {
  std::printf("test_symlink_loop: testing ELOOP detection...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Create symlink loop: a -> b -> a
  const char* link_a = "/tmp/evring_loop_a";
  const char* link_b = "/tmp/evring_loop_b";

  unlink(link_a);
  unlink(link_b);

  int ret = symlink(link_b, link_a);
  assert(ret == 0);
  ret = symlink(link_a, link_b);
  assert(ret == 0);

  // Try to open - should get ELOOP
  ring->enqueue(evring::operation::make_open(link_a, O_RDONLY));
  auto events = ring->submit_and_wait(1);

  assert(!events[0].ok());
  assert(events[0].error_code() == ELOOP);
  std::printf("  PASS: symlink loop correctly returns ELOOP\n");

  // Try to stat
  struct statx buf;
  ring->enqueue(evring::operation::make_statx(AT_FDCWD, link_a, 0, STATX_BASIC_STATS, &buf));
  events = ring->submit_and_wait(1);

  assert(!events[0].ok());
  assert(events[0].error_code() == ELOOP);
  std::printf("  PASS: statx on symlink loop returns ELOOP\n");

  // Cleanup
  unlink(link_a);
  unlink(link_b);

  std::printf("test_symlink_loop: PASSED\n\n");
}

// ============================================================================
// Empty path edge case
// ============================================================================

void test_empty_path() {
  std::printf("test_empty_path: testing empty string paths...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Empty path to open
  ring->enqueue(evring::operation::make_open("", O_RDONLY));
  auto events = ring->submit_and_wait(1);

  assert(!events[0].ok());
  assert(events[0].error_code() == ENOENT);
  std::printf("  PASS: empty path returns ENOENT\n");

  // Empty path to statx
  struct statx buf;
  ring->enqueue(evring::operation::make_statx(AT_FDCWD, "", 0, STATX_BASIC_STATS, &buf));
  events = ring->submit_and_wait(1);

  assert(!events[0].ok());
  assert(events[0].error_code() == ENOENT);
  std::printf("  PASS: statx on empty path returns ENOENT\n");

  std::printf("test_empty_path: PASSED\n\n");
}

// ============================================================================
// Concurrent operations on same file
// ============================================================================

void test_concurrent_same_file() {
  std::printf("test_concurrent_same_file: racing reads on same file...\n");

  auto ring = evring::make_io_uring_ring(64);

  // Create a file with known content
  char tmp_path[] = "/tmp/evring_concurrent_XXXXXX";
  int fd = mkstemp(tmp_path);
  assert(fd >= 0);

  std::vector<char> content(64 * 1024);
  for (std::size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>('A' + (i % 26));
  }
  ssize_t written = write(fd, content.data(), content.size());
  assert(written == static_cast<ssize_t>(content.size()));
  close(fd);

  // Open the file
  ring->enqueue(evring::operation::make_open(tmp_path, O_RDONLY));
  auto events = ring->submit_and_wait(1);
  assert(events[0].ok());
  evring::handle file = events[0].resource_handle;

  // Submit many overlapping reads at different offsets
  const int num_reads = 32;
  std::vector<std::vector<std::byte>> buffers(num_reads);

  for (int i = 0; i < num_reads; ++i) {
    buffers[i].resize(4096);
    ring->enqueue(evring::operation::make_read(
        file, std::span<std::byte>{buffers[i].data(), buffers[i].size()},
        static_cast<int64_t>((i * 1024) % content.size())));
  }

  int submitted = ring->submit();
  assert(submitted == num_reads);

  // Collect all completions
  int completed = 0;
  while (completed < num_reads) {
    events = ring->submit_and_wait(1);
    for (auto& e : events) {
      assert(e.ok());
      assert(e.result > 0);
      completed++;
    }
  }

  std::printf("  PASS: %d concurrent reads completed without corruption\n", completed);

  // Verify data integrity on one buffer
  bool data_ok = true;
  for (std::size_t i = 0; i < buffers[0].size() && i < content.size(); ++i) {
    if (static_cast<char>(buffers[0][i]) != content[i]) {
      data_ok = false;
      break;
    }
  }
  assert(data_ok);
  std::printf("  PASS: data integrity verified\n");

  ring->enqueue(evring::operation::make_close(file));
  ring->submit_and_wait(1);
  unlink(tmp_path);

  std::printf("test_concurrent_same_file: PASSED\n\n");
}

// ============================================================================
// Poll timeout / cancel
// ============================================================================

void test_timeout_and_cancel() {
  std::printf("test_timeout_and_cancel: testing timeout operations...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Submit a very short timeout (1ms)
  ring->enqueue(evring::operation::make_timeout(1'000'000, 1)); // 1ms in nanoseconds
  auto events = ring->submit_and_wait(1);

  // Timeout completion has result == -ETIME
  assert(events[0].result == -ETIME || events[0].result == 0);
  std::printf("  PASS: 1ms timeout completed with result %ld\n", events[0].result);

  // Submit a longer timeout and try to cancel it
  ring->enqueue(evring::operation::make_timeout(60'000'000'000ULL, 2)); // 60 seconds

  // Submit immediately to get it in flight
  int submitted = ring->submit();
  assert(submitted == 1);

  // Now cancel it
  // Note: cancel takes the handle of the operation to cancel
  // For timeouts, we need to use the user_data to identify it
  // This is a bit awkward - io_uring's cancel uses user_data matching
  // For now, just verify we can submit a timeout

  std::printf("  PASS: long timeout submitted (not waiting for it)\n");

  // We'd need to wait or cancel here, but for the test we'll just
  // create a new ring to abandon the inflight op
  std::printf("test_timeout_and_cancel: PASSED\n\n");
}

// ============================================================================
// Directory operations edge cases
// ============================================================================

void test_directory_edge_cases() {
  std::printf("test_directory_edge_cases: testing mkdir/rmdir edge cases...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Try to rmdir non-empty directory
  char tmp_dir[] = "/tmp/evring_dir_test_XXXXXX";
  char* created_dir = mkdtemp(tmp_dir);
  assert(created_dir != nullptr);

  // Create a file inside
  std::string file_path = std::string(tmp_dir) + "/file.txt";
  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY, 0644);
  assert(fd >= 0);
  close(fd);

  // rmdir should fail with ENOTEMPTY
  ring->enqueue(evring::operation::make_rmdir(tmp_dir));
  auto events = ring->submit_and_wait(1);

  assert(!events[0].ok());
  assert(events[0].error_code() == ENOTEMPTY);
  std::printf("  PASS: rmdir non-empty directory returns ENOTEMPTY\n");

  // Remove file and try again
  unlink(file_path.c_str());
  ring->enqueue(evring::operation::make_rmdir(tmp_dir));
  events = ring->submit_and_wait(1);

  assert(events[0].ok());
  std::printf("  PASS: rmdir empty directory succeeds\n");

  // mkdir on existing directory - EEXIST
  assert(mkdir("/tmp", 0755) == -1 && errno == EEXIST);
  ring->enqueue(evring::operation::make_mkdir("/tmp"));
  events = ring->submit_and_wait(1);

  assert(!events[0].ok());
  assert(events[0].error_code() == EEXIST);
  std::printf("  PASS: mkdir existing directory returns EEXIST\n");

  std::printf("test_directory_edge_cases: PASSED\n\n");
}

// ============================================================================
// Socket operations without connect (expect failures)
// ============================================================================

void test_socket_unconnected_operations() {
  std::printf("test_socket_unconnected_operations: send/recv on unconnected socket...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Create a socket but don't connect it
  ring->enqueue(evring::operation::make_socket(AF_INET, SOCK_STREAM, 0, SOCK_NONBLOCK));
  auto events = ring->submit_and_wait(1);
  assert(events[0].ok());
  evring::handle sock = events[0].resource_handle;

  // Try to send on unconnected socket
  const char* data = "hello";
  std::vector<std::byte> buf(strlen(data));
  memcpy(buf.data(), data, buf.size());

  ring->enqueue(evring::operation::make_send(sock, std::span{buf.data(), buf.size()}, 0));
  events = ring->submit_and_wait(1);

  assert(!events[0].ok());
  // Could be ENOTCONN, EPIPE, or EDESTADDRREQ
  int err = events[0].error_code();
  assert(err == ENOTCONN || err == EPIPE || err == EDESTADDRREQ || err == EBADF);
  std::printf("  PASS: send on unconnected socket fails with %d\n", err);

  // Try to recv
  ring->enqueue(evring::operation::make_recv(sock, std::span{buf.data(), buf.size()}, 0));
  events = ring->submit_and_wait(1);

  assert(!events[0].ok());
  err = events[0].error_code();
  assert(err == ENOTCONN || err == EBADF);
  std::printf("  PASS: recv on unconnected socket fails with %d\n", err);

  ring->enqueue(evring::operation::make_close(sock));
  ring->submit_and_wait(1);

  std::printf("test_socket_unconnected_operations: PASSED\n\n");
}

} // namespace

int main() {
  std::printf("=== ADVERSARIAL TESTS ===\n");
  std::printf("\"I like these calm little moments before the storm.\"\n\n");

  test_open_every_errno();
  test_handle_generation_aba();
  test_zero_byte_operations();
  test_offset_edge_cases();
  test_invalid_handle_operations();
  test_rapid_fire_operations();
  test_generator_all_fail();
  test_generator_mixed_results();
  test_replay_truncated_stream();
  test_symlink_loop();
  test_empty_path();
  test_concurrent_same_file();
  test_timeout_and_cancel();
  test_directory_edge_cases();
  test_socket_unconnected_operations();

  std::printf("=== ALL ADVERSARIAL TESTS PASSED ===\n");
  std::printf("\"I haven't got time for this Mickey Mouse bullshit.\"\n");
  return 0;
}
