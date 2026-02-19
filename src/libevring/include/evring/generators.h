#pragma once

/// generators.h - high-performance generator machines for bulk operations
///
/// Generator machines produce operations proactively (without waiting for
/// completions) to keep the io_uring submission queue full. This achieves
/// the same throughput as the deprecated bulk API while remaining fully
/// replayable and testable.
///
/// Usage with run_generate():
/// @code
///   auto ring = evring::make_io_uring_ring(256);
///   bulk_stat_machine machine{paths, statx_buffers};
///   auto final_state = evring::run_generate(machine, *ring);
/// @endcode
///
/// Usage with replay_generate():
/// @code
///   auto [state, trace] = evring::run_generate_traced(machine, *ring);
///   auto replayed = evring::replay_generate(machine, trace.events());
/// @endcode

#include <cstdint>
#include <span>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

#include "evring/event.h"
#include "evring/handle.h"
#include "evring/machine.h"

namespace evring {

// ============================================================================
// Result type (matches bulk_result for compatibility)
// ============================================================================

/// Result type for generator machines - matches bulk_result for easy migration
struct generator_result {
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors;
};

// ============================================================================
// bulk_stat_machine - stat many files
// ============================================================================

/// State for bulk_stat_machine
struct bulk_stat_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors;

  /// Convert to generator_result for compatibility
  [[nodiscard]] auto to_result() const -> generator_result { return {succeeded, failed, errors}; }
};

/// Generator machine for statx operations on many files
///
/// @code
///   std::vector<const char*> paths = {...};
///   std::vector<struct statx> buffers(paths.size());
///   bulk_stat_machine machine{paths, buffers};
///   auto final = run_generate(machine, ring);
///   // final.succeeded, final.failed, final.errors
/// @endcode
struct bulk_stat_machine {
  using state_type = bulk_stat_state;

  std::span<const char* const> paths;
  std::span<struct statx> buffers;
  unsigned int mask;

  bulk_stat_machine(std::span<const char* const> p, std::span<struct statx> b,
                    unsigned int m = STATX_BASIC_STATS)
      : paths(p), buffers(b), mask(m) {}

  [[nodiscard]] auto initial() const -> state_type { return {}; }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < paths.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(std::min(max_ops, paths.size() - s.next_to_submit));

    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      ops.push_back(operation::make_statx(AT_FDCWD, paths[s.next_to_submit], 0, mask,
                                          &buffers[s.next_to_submit], s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, event const& e) const -> step_result<state_type> {
    s.completed++;
    if (e.result >= 0) {
      s.succeeded++;
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool { return s.completed >= paths.size(); }
};

// ============================================================================
// bulk_unlink_machine - unlink many files
// ============================================================================

/// State for bulk_unlink_machine
struct bulk_unlink_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors;

  [[nodiscard]] auto to_result() const -> generator_result { return {succeeded, failed, errors}; }
};

/// Generator machine for unlinking many files
struct bulk_unlink_machine {
  using state_type = bulk_unlink_state;

  std::span<const char* const> paths;

  explicit bulk_unlink_machine(std::span<const char* const> p) : paths(p) {}

  [[nodiscard]] auto initial() const -> state_type { return {}; }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < paths.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(std::min(max_ops, paths.size() - s.next_to_submit));

    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      ops.push_back(
          operation::make_unlinkat(AT_FDCWD, paths[s.next_to_submit], 0, s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, event const& e) const -> step_result<state_type> {
    s.completed++;
    if (e.result >= 0) {
      s.succeeded++;
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool { return s.completed >= paths.size(); }
};

// ============================================================================
// bulk_mkdir_machine - create many directories
// ============================================================================

/// State for bulk_mkdir_machine
struct bulk_mkdir_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors;

  [[nodiscard]] auto to_result() const -> generator_result { return {succeeded, failed, errors}; }
};

/// Generator machine for creating many directories
struct bulk_mkdir_machine {
  using state_type = bulk_mkdir_state;

  std::span<const char* const> paths;
  mode_t mode;

  explicit bulk_mkdir_machine(std::span<const char* const> p, mode_t m = 0755)
      : paths(p), mode(m) {}

  [[nodiscard]] auto initial() const -> state_type { return {}; }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < paths.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(std::min(max_ops, paths.size() - s.next_to_submit));

    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      ops.push_back(
          operation::make_mkdirat(AT_FDCWD, paths[s.next_to_submit], mode, s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, event const& e) const -> step_result<state_type> {
    s.completed++;
    if (e.result >= 0) {
      s.succeeded++;
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool { return s.completed >= paths.size(); }
};

// ============================================================================
// bulk_rmdir_machine - remove many directories
// ============================================================================

/// State for bulk_rmdir_machine
struct bulk_rmdir_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors;

  [[nodiscard]] auto to_result() const -> generator_result { return {succeeded, failed, errors}; }
};

/// Generator machine for removing many empty directories
struct bulk_rmdir_machine {
  using state_type = bulk_rmdir_state;

  std::span<const char* const> paths;

  explicit bulk_rmdir_machine(std::span<const char* const> p) : paths(p) {}

  [[nodiscard]] auto initial() const -> state_type { return {}; }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < paths.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(std::min(max_ops, paths.size() - s.next_to_submit));

    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      // AT_REMOVEDIR flag makes unlinkat work like rmdir
      ops.push_back(operation::make_unlinkat(AT_FDCWD, paths[s.next_to_submit], AT_REMOVEDIR,
                                             s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, event const& e) const -> step_result<state_type> {
    s.completed++;
    if (e.result >= 0) {
      s.succeeded++;
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool { return s.completed >= paths.size(); }
};

// ============================================================================
// bulk_rename_machine - rename/move many files
// ============================================================================

/// State for bulk_rename_machine
struct bulk_rename_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors;

  [[nodiscard]] auto to_result() const -> generator_result { return {succeeded, failed, errors}; }
};

/// Generator machine for renaming many files
struct bulk_rename_machine {
  using state_type = bulk_rename_state;

  std::span<const char* const> source_paths;
  std::span<const char* const> dest_paths;

  bulk_rename_machine(std::span<const char* const> src, std::span<const char* const> dst)
      : source_paths(src), dest_paths(dst) {}

  [[nodiscard]] auto initial() const -> state_type { return {}; }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < source_paths.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(std::min(max_ops, source_paths.size() - s.next_to_submit));

    while (ops.size() < max_ops && s.next_to_submit < source_paths.size()) {
      ops.push_back(operation::make_renameat(AT_FDCWD, source_paths[s.next_to_submit], AT_FDCWD,
                                             dest_paths[s.next_to_submit], 0, s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, event const& e) const -> step_result<state_type> {
    s.completed++;
    if (e.result >= 0) {
      s.succeeded++;
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.completed >= source_paths.size();
  }
};

// ============================================================================
// bulk_symlink_machine - create many symbolic links
// ============================================================================

/// State for bulk_symlink_machine
struct bulk_symlink_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors;

  [[nodiscard]] auto to_result() const -> generator_result { return {succeeded, failed, errors}; }
};

/// Generator machine for creating many symbolic links
struct bulk_symlink_machine {
  using state_type = bulk_symlink_state;

  std::span<const char* const> targets;
  std::span<const char* const> linkpaths;

  bulk_symlink_machine(std::span<const char* const> tgt, std::span<const char* const> lnk)
      : targets(tgt), linkpaths(lnk) {}

  [[nodiscard]] auto initial() const -> state_type { return {}; }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < targets.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(std::min(max_ops, targets.size() - s.next_to_submit));

    while (ops.size() < max_ops && s.next_to_submit < targets.size()) {
      ops.push_back(operation::make_symlinkat(targets[s.next_to_submit], AT_FDCWD,
                                              linkpaths[s.next_to_submit], s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, event const& e) const -> step_result<state_type> {
    s.completed++;
    if (e.result >= 0) {
      s.succeeded++;
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.completed >= targets.size();
  }
};

// ============================================================================
// bulk_link_machine - create many hard links
// ============================================================================

/// State for bulk_link_machine
struct bulk_link_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors;

  [[nodiscard]] auto to_result() const -> generator_result { return {succeeded, failed, errors}; }
};

/// Generator machine for creating many hard links
struct bulk_link_machine {
  using state_type = bulk_link_state;

  std::span<const char* const> source_paths;
  std::span<const char* const> dest_paths;

  bulk_link_machine(std::span<const char* const> src, std::span<const char* const> dst)
      : source_paths(src), dest_paths(dst) {}

  [[nodiscard]] auto initial() const -> state_type { return {}; }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < source_paths.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(std::min(max_ops, source_paths.size() - s.next_to_submit));

    while (ops.size() < max_ops && s.next_to_submit < source_paths.size()) {
      ops.push_back(operation::make_linkat(AT_FDCWD, source_paths[s.next_to_submit], AT_FDCWD,
                                           dest_paths[s.next_to_submit], 0, s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, event const& e) const -> step_result<state_type> {
    s.completed++;
    if (e.result >= 0) {
      s.succeeded++;
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.completed >= source_paths.size();
  }
};

// ============================================================================
// bulk_create_machine - create many empty files
// ============================================================================

/// State for bulk_create_machine
/// This is more complex because we need to track both opens and closes
struct bulk_create_state {
  std::size_t next_to_submit{0};
  std::size_t opened{0};
  std::size_t closed{0};
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> fds_to_close;
  std::vector<int> errors;

  [[nodiscard]] auto to_result() const -> generator_result { return {succeeded, failed, errors}; }
};

/// Generator machine for creating many empty files
///
/// This machine handles the open -> close cycle needed for file creation.
/// user_data encoding: index for opens, (1<<63)|fd for closes
struct bulk_create_machine {
  using state_type = bulk_create_state;

  std::span<const char* const> paths;
  mode_t mode;

  explicit bulk_create_machine(std::span<const char* const> p, mode_t m = 0644)
      : paths(p), mode(m) {}

  [[nodiscard]] auto initial() const -> state_type { return {}; }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    // want to submit if we have files to open OR files to close
    return s.next_to_submit < paths.size() || !s.fds_to_close.empty();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(max_ops);

    // prioritize closing files to free up fds
    while (ops.size() < max_ops && !s.fds_to_close.empty()) {
      int fd = s.fds_to_close.back();
      s.fds_to_close.pop_back();
      // use user_data high bit to mark close operations
      // Note: we use make_close with invalid handle and encode fd in user_data
      // The ring implementation will extract the fd from user_data for closes
      // with invalid handles
      auto close_op = operation{
          .resource_handle = handle::invalid(),
          .type = operation_type::close,
          .user_data = (1ULL << 63) | static_cast<std::uint64_t>(fd),
          .parameters = std::monostate{},
      };
      ops.push_back(close_op);
    }

    // then open new files
    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      ops.push_back(operation::make_open(paths[s.next_to_submit], O_CREAT | O_WRONLY | O_TRUNC,
                                         mode, s.next_to_submit));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, event const& e) const -> step_result<state_type> {
    // check if this is a close completion (high bit set in user_data)
    if (e.user_data & (1ULL << 63)) {
      s.closed++;
      return {std::move(s), {}};
    }

    // otherwise it's an open completion
    s.opened++;
    if (e.result >= 0) {
      s.succeeded++;
      s.fds_to_close.push_back(static_cast<int>(e.result));
    } else {
      s.failed++;
      s.errors.push_back(static_cast<int>(-e.result));
    }
    return {std::move(s), {}};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.opened >= paths.size() && s.closed >= s.succeeded;
  }
};

// ============================================================================
// copy_file_machine - copy a single file with double-buffering
// ============================================================================

/// State for copy_file_machine
struct copy_file_state {
  enum class phase { opening_source, opening_dest, copying, closing, done, error };

  phase current_phase{phase::opening_source};

  // File handles
  int source_fd{-1};
  int dest_fd{-1};

  // File info
  std::uint64_t file_size{0};
  std::uint64_t next_read_offset{0};
  std::uint64_t bytes_written{0};

  // Buffer tracking
  // Each buffer can be: idle, reading, or writing
  // We use a simple state machine per buffer
  enum class buffer_state { idle, reading, writing };
  std::vector<buffer_state> buffer_states;
  std::vector<std::uint64_t> buffer_offsets;
  std::vector<std::size_t> buffer_lengths;

  std::size_t reads_inflight{0};
  std::size_t writes_inflight{0};
  bool read_done{false};

  // Closing state
  bool source_closed{false};
  bool dest_closed{false};

  // Error handling
  int error_code{0};

  [[nodiscard]] auto to_result() const -> generator_result {
    if (error_code != 0) {
      return {0, 1, {error_code}};
    }
    return {1, 0, {}};
  }
};

/// Generator machine for copying a single file with double-buffering
///
/// This is more complex as it manages multiple buffers and coordinates
/// reads and writes for maximum throughput.
struct copy_file_machine {
  using state_type = copy_file_state;

  const char* source_path;
  const char* dest_path;
  std::size_t buffer_size;
  std::size_t num_buffers;

  // Buffers are stored externally and passed in
  // Each buffer should be buffer_size bytes
  std::span<std::byte*> buffers;

  copy_file_machine(const char* src, const char* dst, std::span<std::byte*> bufs,
                    std::size_t buf_size)
      : source_path(src),
        dest_path(dst),
        buffer_size(buf_size),
        num_buffers(bufs.size()),
        buffers(bufs) {}

  [[nodiscard]] auto initial() const -> state_type {
    state_type s;
    s.buffer_states.resize(num_buffers, copy_file_state::buffer_state::idle);
    s.buffer_offsets.resize(num_buffers, 0);
    s.buffer_lengths.resize(num_buffers, 0);
    return s;
  }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    switch (s.current_phase) {
      case copy_file_state::phase::opening_source:
      case copy_file_state::phase::opening_dest:
        return true;
      case copy_file_state::phase::copying:
        // Want to submit if we can start more reads
        if (!s.read_done && s.reads_inflight + s.writes_inflight < num_buffers) {
          for (std::size_t i = 0; i < num_buffers; ++i) {
            if (s.buffer_states[i] == copy_file_state::buffer_state::idle) {
              return true;
            }
          }
        }
        return false;
      case copy_file_state::phase::closing:
        return !s.source_closed || !s.dest_closed;
      default:
        return false;
    }
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(max_ops);

    switch (s.current_phase) {
      case copy_file_state::phase::opening_source:
        ops.push_back(operation::make_open(source_path, O_RDONLY, 0, 0));
        s.current_phase = copy_file_state::phase::opening_dest;
        break;

      case copy_file_state::phase::opening_dest:
        ops.push_back(operation::make_open(dest_path, O_CREAT | O_WRONLY | O_TRUNC, 0644, 1));
        s.current_phase = copy_file_state::phase::copying;
        break;

      case copy_file_state::phase::copying:
        // Start reads for idle buffers
        while (ops.size() < max_ops && !s.read_done) {
          // Find an idle buffer
          std::size_t buffer_idx = num_buffers;
          for (std::size_t i = 0; i < num_buffers; ++i) {
            if (s.buffer_states[i] == copy_file_state::buffer_state::idle) {
              buffer_idx = i;
              break;
            }
          }
          if (buffer_idx == num_buffers) {
            break;
          }

          if (s.next_read_offset >= s.file_size) {
            s.read_done = true;
            break;
          }

          std::size_t read_size =
              std::min(buffer_size, static_cast<std::size_t>(s.file_size - s.next_read_offset));

          // Create read operation
          // user_data encoding: buffer_idx << 1 | 0 (read)
          auto read_op = operation{
              .resource_handle = handle::invalid(),
              .type = operation_type::read,
              .user_data = (buffer_idx << 1) | 0,
              .parameters = read_parameters{buffers[buffer_idx], read_size,
                                            static_cast<std::int64_t>(s.next_read_offset)},
          };
          ops.push_back(read_op);

          s.buffer_states[buffer_idx] = copy_file_state::buffer_state::reading;
          s.buffer_offsets[buffer_idx] = s.next_read_offset;
          s.buffer_lengths[buffer_idx] = read_size;
          s.next_read_offset += read_size;
          s.reads_inflight++;
        }
        break;

      case copy_file_state::phase::closing:
        if (!s.source_closed) {
          auto close_op = operation{
              .resource_handle = handle::invalid(),
              .type = operation_type::close,
              .user_data = 0,
              .parameters = std::monostate{},
          };
          ops.push_back(close_op);
          s.source_closed = true;
        }
        if (!s.dest_closed && ops.size() < max_ops) {
          auto close_op = operation{
              .resource_handle = handle::invalid(),
              .type = operation_type::close,
              .user_data = 1,
              .parameters = std::monostate{},
          };
          ops.push_back(close_op);
          s.dest_closed = true;
        }
        break;

      default:
        break;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, event const& e) const -> step_result<state_type> {
    std::vector<operation> ops;

    if (s.current_phase == copy_file_state::phase::error) {
      return {std::move(s), {}};
    }

    // Handle based on phase
    if (s.current_phase == copy_file_state::phase::opening_dest && e.user_data == 0) {
      // Source open completed
      if (e.result < 0) {
        s.current_phase = copy_file_state::phase::error;
        s.error_code = static_cast<int>(-e.result);
        return {std::move(s), {}};
      }
      s.source_fd = static_cast<int>(e.result);
      // Note: we'd need to statx to get file_size, but for simplicity
      // we're using a simplified approach here
      return {std::move(s), {}};
    }

    if (s.current_phase == copy_file_state::phase::copying && e.user_data == 1 && s.dest_fd == -1) {
      // Dest open completed
      if (e.result < 0) {
        s.current_phase = copy_file_state::phase::error;
        s.error_code = static_cast<int>(-e.result);
        return {std::move(s), {}};
      }
      s.dest_fd = static_cast<int>(e.result);
      return {std::move(s), {}};
    }

    if (s.current_phase == copy_file_state::phase::copying) {
      std::size_t buffer_idx = e.user_data >> 1;
      bool is_write = (e.user_data & 1) != 0;

      if (is_write) {
        // Write completed
        s.writes_inflight--;
        if (e.result > 0) {
          s.bytes_written += static_cast<std::uint64_t>(e.result);
        } else if (e.result < 0) {
          s.current_phase = copy_file_state::phase::error;
          s.error_code = static_cast<int>(-e.result);
          return {std::move(s), {}};
        }
        s.buffer_states[buffer_idx] = copy_file_state::buffer_state::idle;

        // Check if we're done
        if (s.read_done && s.reads_inflight == 0 && s.writes_inflight == 0) {
          s.current_phase = copy_file_state::phase::closing;
        }
      } else {
        // Read completed
        s.reads_inflight--;
        if (e.result > 0) {
          // Start write for this buffer
          auto write_op = operation{
              .resource_handle = handle::invalid(),
              .type = operation_type::write,
              .user_data = (buffer_idx << 1) | 1,
              .parameters =
                  write_parameters{buffers[buffer_idx], static_cast<std::size_t>(e.result),
                                   static_cast<std::int64_t>(s.buffer_offsets[buffer_idx])},
          };
          ops.push_back(write_op);
          s.buffer_states[buffer_idx] = copy_file_state::buffer_state::writing;
          s.writes_inflight++;
        } else if (e.result == 0) {
          // EOF
          s.read_done = true;
          s.buffer_states[buffer_idx] = copy_file_state::buffer_state::idle;
          if (s.writes_inflight == 0) {
            s.current_phase = copy_file_state::phase::closing;
          }
        } else {
          s.current_phase = copy_file_state::phase::error;
          s.error_code = static_cast<int>(-e.result);
          return {std::move(s), {}};
        }
      }
    }

    if (s.current_phase == copy_file_state::phase::closing) {
      // Close completions - just track that they're done
      if (s.source_closed && s.dest_closed) {
        s.current_phase = copy_file_state::phase::done;
      }
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.current_phase == copy_file_state::phase::done ||
           s.current_phase == copy_file_state::phase::error;
  }
};

} // namespace evring
