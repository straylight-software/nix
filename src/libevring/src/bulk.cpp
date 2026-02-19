/// bulk.cpp - high-performance bulk operations implementation
///
/// Key optimizations:
/// 1. Direct io_uring access (no state machine overhead)
/// 2. Keep SQ full at all times (max inflight operations)
/// 3. Batch completion processing
/// 4. Minimal memory allocation in hot path

#include "evring/bulk.h"

#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <liburing.h>
#include <linux/stat.h>
#include <sys/stat.h>
#include <unistd.h>

#include "evring/ring.h"

namespace evring {

namespace {

/// direct io_uring context for bulk operations
/// this bypasses the ring abstraction for maximum performance
struct bulk_context {
  struct io_uring ring;
  unsigned depth;

  explicit bulk_context(unsigned entries) : depth(entries) {
    int result = io_uring_queue_init(entries, &ring, 0);
    if (result < 0) {
      throw std::runtime_error(std::string("io_uring_queue_init failed: ") +
                               std::strerror(-result));
    }
  }

  ~bulk_context() { io_uring_queue_exit(&ring); }

  bulk_context(bulk_context const&) = delete;
  bulk_context& operator=(bulk_context const&) = delete;
};

/// submit all pending and wait for at least one completion
/// returns number of completions available
auto submit_and_wait(bulk_context& context, int min_completions = 1) -> int {
  int result = io_uring_submit_and_wait(&context.ring, min_completions);
  if (result < 0) {
    throw std::runtime_error(std::string("io_uring_submit_and_wait failed: ") +
                             std::strerror(-result));
  }
  return result;
}

/// process all available completions, calling handler for each
/// returns number of completions processed
template <typename Handler>
auto harvest_completions(bulk_context& context, Handler&& handler) -> unsigned {
  struct io_uring_cqe* cqe;
  unsigned head;
  unsigned count = 0;

  io_uring_for_each_cqe(&context.ring, head, cqe) {
    handler(io_uring_cqe_get_data64(cqe), cqe->res);
    ++count;
  }

  io_uring_cq_advance(&context.ring, count);
  return count;
}

} // namespace

// ============================================================================
// Bulk file creation
// ============================================================================

auto bulk_create_files(ring& /*ring_instance*/, std::span<const char* const> paths, mode_t mode)
    -> bulk_result {
  if (paths.empty()) {
    return bulk_result{};
  }

  // use our own ring for maximum control
  bulk_context context(256);
  bulk_result result;

  std::size_t submitted = 0;
  std::size_t completed = 0;
  std::size_t const total = paths.size();

  // track which files need to be closed after creation
  // index -> fd mapping for files that were successfully opened
  std::vector<int> fds_to_close;
  fds_to_close.reserve(std::min(total, static_cast<std::size_t>(256)));

  // phase 1: create files (open with O_CREAT)
  while (completed < total) {
    // fill the SQ as much as possible
    while (submitted < total) {
      struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
      if (sqe == nullptr) {
        break; // SQ full, need to submit and harvest
      }

      io_uring_prep_openat(sqe, AT_FDCWD, paths[submitted], O_CREAT | O_WRONLY | O_TRUNC, mode);
      io_uring_sqe_set_data64(sqe, submitted);
      ++submitted;
    }

    // submit and wait for completions
    submit_and_wait(context, 1);

    // harvest completions
    harvest_completions(context, [&](std::uint64_t /*index*/, int res) {
      ++completed;
      if (res >= 0) {
        ++result.succeeded;
        fds_to_close.push_back(res);
      } else {
        ++result.failed;
        result.errors.push_back(-res);
      }
    });

    // close files in batches to avoid accumulating too many open fds
    if (fds_to_close.size() >= 128) {
      std::size_t close_submitted = 0;
      std::size_t close_completed = 0;
      std::size_t const close_total = fds_to_close.size();

      while (close_completed < close_total) {
        while (close_submitted < close_total) {
          struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
          if (sqe == nullptr) {
            break;
          }
          io_uring_prep_close(sqe, fds_to_close[close_submitted]);
          io_uring_sqe_set_data64(sqe, close_submitted);
          ++close_submitted;
        }

        submit_and_wait(context, 1);
        harvest_completions(context,
                            [&](std::uint64_t /*index*/, int /*res*/) { ++close_completed; });
      }

      fds_to_close.clear();
    }
  }

  // close remaining files
  if (!fds_to_close.empty()) {
    std::size_t close_submitted = 0;
    std::size_t close_completed = 0;
    std::size_t const close_total = fds_to_close.size();

    while (close_completed < close_total) {
      while (close_submitted < close_total) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
        if (sqe == nullptr) {
          break;
        }
        io_uring_prep_close(sqe, fds_to_close[close_submitted]);
        io_uring_sqe_set_data64(sqe, close_submitted);
        ++close_submitted;
      }

      submit_and_wait(context, 1);
      harvest_completions(context,
                          [&](std::uint64_t /*index*/, int /*res*/) { ++close_completed; });
    }
  }

  return result;
}

auto bulk_create_files(ring& ring_instance, std::span<const std::string> paths, mode_t mode)
    -> bulk_result {
  std::vector<const char*> c_paths;
  c_paths.reserve(paths.size());
  for (auto const& path : paths) {
    c_paths.push_back(path.c_str());
  }
  return bulk_create_files(ring_instance, std::span<const char* const>(c_paths), mode);
}

// ============================================================================
// Bulk stat
// ============================================================================

auto bulk_stat(ring& /*ring_instance*/, std::span<const char* const> paths,
               std::span<struct statx> statx_buffers, unsigned int mask) -> bulk_result {
  if (paths.empty()) {
    return bulk_result{};
  }

  if (paths.size() != statx_buffers.size()) {
    throw std::runtime_error("paths and statx_buffers must have same size");
  }

  bulk_context context(256);
  bulk_result result;

  std::size_t submitted = 0;
  std::size_t completed = 0;
  std::size_t const total = paths.size();

  while (completed < total) {
    // fill the SQ
    while (submitted < total) {
      struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
      if (sqe == nullptr) {
        break;
      }

      io_uring_prep_statx(sqe, AT_FDCWD, paths[submitted], 0, mask, &statx_buffers[submitted]);
      io_uring_sqe_set_data64(sqe, submitted);
      ++submitted;
    }

    submit_and_wait(context, 1);

    harvest_completions(context, [&](std::uint64_t /*index*/, int res) {
      ++completed;
      if (res >= 0) {
        ++result.succeeded;
      } else {
        ++result.failed;
        result.errors.push_back(-res);
      }
    });
  }

  return result;
}

auto bulk_stat(ring& ring_instance, std::span<const std::string> paths,
               std::span<struct statx> statx_buffers, unsigned int mask) -> bulk_result {
  std::vector<const char*> c_paths;
  c_paths.reserve(paths.size());
  for (auto const& path : paths) {
    c_paths.push_back(path.c_str());
  }
  return bulk_stat(ring_instance, std::span<const char* const>(c_paths), statx_buffers, mask);
}

// ============================================================================
// Bulk unlink
// ============================================================================

auto bulk_unlink(ring& /*ring_instance*/, std::span<const char* const> paths) -> bulk_result {
  if (paths.empty()) {
    return bulk_result{};
  }

  bulk_context context(256);
  bulk_result result;

  std::size_t submitted = 0;
  std::size_t completed = 0;
  std::size_t const total = paths.size();

  while (completed < total) {
    while (submitted < total) {
      struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
      if (sqe == nullptr) {
        break;
      }

      io_uring_prep_unlinkat(sqe, AT_FDCWD, paths[submitted], 0);
      io_uring_sqe_set_data64(sqe, submitted);
      ++submitted;
    }

    submit_and_wait(context, 1);

    harvest_completions(context, [&](std::uint64_t /*index*/, int res) {
      ++completed;
      if (res >= 0) {
        ++result.succeeded;
      } else {
        ++result.failed;
        result.errors.push_back(-res);
      }
    });
  }

  return result;
}

auto bulk_unlink(ring& ring_instance, std::span<const std::string> paths) -> bulk_result {
  std::vector<const char*> c_paths;
  c_paths.reserve(paths.size());
  for (auto const& path : paths) {
    c_paths.push_back(path.c_str());
  }
  return bulk_unlink(ring_instance, std::span<const char* const>(c_paths));
}

// ============================================================================
// File copy with double-buffering
// ============================================================================

auto copy_file(ring& /*ring_instance*/, const char* source, const char* dest,
               copy_options const& options) -> bulk_result {
  bulk_result result;

  // open source
  int const source_fd = open(source, O_RDONLY | (options.use_direct_io ? O_DIRECT : 0));
  if (source_fd < 0) {
    result.failed = 1;
    result.errors.push_back(errno);
    return result;
  }

  // get file size
  struct stat stat_buffer;
  if (fstat(source_fd, &stat_buffer) < 0) {
    close(source_fd);
    result.failed = 1;
    result.errors.push_back(errno);
    return result;
  }
  std::uint64_t const file_size = static_cast<std::uint64_t>(stat_buffer.st_size);

  // open dest
  int const dest_fd =
      open(dest, O_CREAT | O_WRONLY | O_TRUNC | (options.use_direct_io ? O_DIRECT : 0), 0644);
  if (dest_fd < 0) {
    close(source_fd);
    result.failed = 1;
    result.errors.push_back(errno);
    return result;
  }

  // allocate buffers for double-buffering
  // we use multiple buffer pairs to keep the ring full
  std::size_t const buffer_size = options.buffer_size;
  std::size_t const num_buffers = options.ring_depth;

  // aligned allocation for O_DIRECT
  std::vector<std::unique_ptr<std::byte[], void (*)(void*)>> buffers;
  buffers.reserve(num_buffers);
  for (std::size_t index = 0; index < num_buffers; ++index) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 4096, buffer_size) != 0) {
      close(source_fd);
      close(dest_fd);
      result.failed = 1;
      result.errors.push_back(ENOMEM);
      return result;
    }
    buffers.emplace_back(static_cast<std::byte*>(ptr), free);
  }

  // io_uring setup
  bulk_context context(static_cast<unsigned>(num_buffers * 2 + 8));

  // state tracking
  enum class buffer_state { idle, reading, writing };
  struct buffer_info {
    buffer_state state = buffer_state::idle;
    std::uint64_t offset = 0;
    std::size_t length = 0;
  };
  std::vector<buffer_info> buffer_states(num_buffers);

  std::uint64_t next_read_offset = 0;
  std::uint64_t bytes_written = 0;
  std::size_t reads_inflight = 0;
  std::size_t writes_inflight = 0;
  bool read_done = false;

  // tag encoding: buffer_index << 1 | is_write
  auto make_tag = [](std::size_t buffer_index, bool is_write) -> std::uint64_t {
    return (buffer_index << 1) | (is_write ? 1 : 0);
  };
  auto parse_tag = [](std::uint64_t tag) -> std::pair<std::size_t, bool> {
    return {tag >> 1, (tag & 1) != 0};
  };

  // start initial reads
  auto start_reads = [&]() {
    while (!read_done && reads_inflight + writes_inflight < num_buffers) {
      // find an idle buffer
      std::size_t buffer_index = num_buffers;
      for (std::size_t index = 0; index < num_buffers; ++index) {
        if (buffer_states[index].state == buffer_state::idle) {
          buffer_index = index;
          break;
        }
      }
      if (buffer_index == num_buffers) {
        break; // no idle buffers
      }

      // check if we've read everything
      if (next_read_offset >= file_size) {
        read_done = true;
        break;
      }

      struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
      if (sqe == nullptr) {
        break;
      }

      std::size_t const read_size =
          std::min(buffer_size, static_cast<std::size_t>(file_size - next_read_offset));

      io_uring_prep_read(sqe, source_fd, buffers[buffer_index].get(), read_size, next_read_offset);
      io_uring_sqe_set_data64(sqe, make_tag(buffer_index, false));

      buffer_states[buffer_index].state = buffer_state::reading;
      buffer_states[buffer_index].offset = next_read_offset;
      buffer_states[buffer_index].length = read_size;

      next_read_offset += read_size;
      ++reads_inflight;
    }
  };

  // main loop
  start_reads();

  while (bytes_written < file_size || reads_inflight > 0 || writes_inflight > 0) {
    if (reads_inflight > 0 || writes_inflight > 0) {
      submit_and_wait(context, 1);
    }

    harvest_completions(context, [&](std::uint64_t tag, int res) {
      auto [buffer_index, is_write] = parse_tag(tag);

      if (is_write) {
        // write completed
        --writes_inflight;
        if (res > 0) {
          bytes_written += static_cast<std::uint64_t>(res);
          if (options.on_progress) {
            options.on_progress(bytes_written);
          }
        } else if (res < 0) {
          result.failed = 1;
          result.errors.push_back(-res);
        }
        buffer_states[buffer_index].state = buffer_state::idle;
      } else {
        // read completed
        --reads_inflight;
        if (res > 0) {
          // start write for this buffer
          struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
          if (sqe != nullptr) {
            io_uring_prep_write(sqe, dest_fd, buffers[buffer_index].get(),
                                static_cast<std::size_t>(res), buffer_states[buffer_index].offset);
            io_uring_sqe_set_data64(sqe, make_tag(buffer_index, true));
            buffer_states[buffer_index].state = buffer_state::writing;
            ++writes_inflight;
          } else {
            // SQ full - this shouldn't happen with proper sizing
            buffer_states[buffer_index].state = buffer_state::idle;
          }
        } else if (res == 0) {
          // EOF
          read_done = true;
          buffer_states[buffer_index].state = buffer_state::idle;
        } else {
          result.failed = 1;
          result.errors.push_back(-res);
          buffer_states[buffer_index].state = buffer_state::idle;
        }
      }
    });

    // start more reads if possible
    start_reads();
  }

  close(source_fd);
  close(dest_fd);

  if (result.failed == 0) {
    result.succeeded = 1;
  }

  return result;
}

auto copy_file(ring& ring_instance, std::string const& source, std::string const& dest,
               copy_options const& options) -> bulk_result {
  return copy_file(ring_instance, source.c_str(), dest.c_str(), options);
}

// ============================================================================
// Bulk mkdir
// ============================================================================

auto bulk_mkdir(ring& /*ring_instance*/, std::span<const char* const> paths, mode_t mode)
    -> bulk_result {
  if (paths.empty()) {
    return bulk_result{};
  }

  bulk_context context(256);
  bulk_result result;

  std::size_t submitted = 0;
  std::size_t completed = 0;
  std::size_t const total = paths.size();

  while (completed < total) {
    while (submitted < total) {
      struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
      if (sqe == nullptr) {
        break;
      }

      io_uring_prep_mkdirat(sqe, AT_FDCWD, paths[submitted], mode);
      io_uring_sqe_set_data64(sqe, submitted);
      ++submitted;
    }

    submit_and_wait(context, 1);

    harvest_completions(context, [&](std::uint64_t /*index*/, int res) {
      ++completed;
      if (res >= 0) {
        ++result.succeeded;
      } else {
        ++result.failed;
        result.errors.push_back(-res);
      }
    });
  }

  return result;
}

auto bulk_mkdir(ring& ring_instance, std::span<const std::string> paths, mode_t mode)
    -> bulk_result {
  std::vector<const char*> c_paths;
  c_paths.reserve(paths.size());
  for (auto const& path : paths) {
    c_paths.push_back(path.c_str());
  }
  return bulk_mkdir(ring_instance, std::span<const char* const>(c_paths), mode);
}

} // namespace evring
