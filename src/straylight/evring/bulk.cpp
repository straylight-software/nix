/// bulk.cpp - high-performance bulk operations implementation
///
/// Key optimizations:
/// 1. Direct io_uring access (no state machine overhead)
/// 2. Keep SQ full at all times (max inflight operations)
/// 3. Batch completion processing
/// 4. Minimal memory allocation in hot path

#include "straylight/evring/bulk.h"

#include <cstring>
#include <stdexcept>

#include <dirent.h>
#include <fcntl.h>
#include <liburing.h>
#include <sys/stat.h>
#include <unistd.h>

#include "straylight/evring/ring.h"

namespace evring {

namespace {

/// bulk operation flags
enum class bulk_flags : unsigned {
  none = 0,
  sqpoll = 1 << 0, // use SQPOLL mode
};

constexpr auto operator|(bulk_flags lhs, bulk_flags rhs) -> bulk_flags {
  return static_cast<bulk_flags>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

constexpr auto operator&(bulk_flags lhs, bulk_flags rhs) -> bulk_flags {
  return static_cast<bulk_flags>(static_cast<unsigned>(lhs) & static_cast<unsigned>(rhs));
}

/// direct io_uring context for bulk operations
/// this bypasses the ring abstraction for maximum performance
struct bulk_context {
  struct io_uring ring;
  unsigned depth;
  bool using_sqpoll;

  explicit bulk_context(unsigned entries, bulk_flags flags = bulk_flags::none)
      : depth(entries), using_sqpoll((flags & bulk_flags::sqpoll) != bulk_flags::none) {
    unsigned io_flags = 0;
    if (using_sqpoll) {
      io_flags |= IORING_SETUP_SQPOLL;
    }

    if (using_sqpoll) {
      // use params for SQPOLL to set idle timeout
      struct io_uring_params params{};
      params.flags = io_flags;
      params.sq_thread_idle = 1000; // 1 second idle before sleep
      int result = io_uring_queue_init_params(entries, &ring, &params);
      if (result < 0) {
        throw std::runtime_error(std::string("io_uring_queue_init_params failed: ") +
                                 std::strerror(-result));
      }
    } else {
      int result = io_uring_queue_init(entries, &ring, io_flags);
      if (result < 0) {
        throw std::runtime_error(std::string("io_uring_queue_init failed: ") +
                                 std::strerror(-result));
      }
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

// ============================================================================
// Bulk rmdir
// ============================================================================

auto bulk_rmdir(ring& /*ring_instance*/, std::span<const char* const> paths) -> bulk_result {
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

      io_uring_prep_unlinkat(sqe, AT_FDCWD, paths[submitted], AT_REMOVEDIR);
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

auto bulk_rmdir(ring& ring_instance, std::span<const std::string> paths) -> bulk_result {
  std::vector<const char*> c_paths;
  c_paths.reserve(paths.size());
  for (auto const& path : paths) {
    c_paths.push_back(path.c_str());
  }
  return bulk_rmdir(ring_instance, std::span<const char* const>(c_paths));
}

// ============================================================================
// Bulk rename
// ============================================================================

auto bulk_rename(ring& /*ring_instance*/, std::span<const char* const> source_paths,
                 std::span<const char* const> dest_paths) -> bulk_result {
  if (source_paths.empty() || source_paths.size() != dest_paths.size()) {
    return bulk_result{};
  }

  bulk_context context(256);
  bulk_result result;

  std::size_t submitted = 0;
  std::size_t completed = 0;
  std::size_t const total = source_paths.size();

  while (completed < total) {
    while (submitted < total) {
      struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
      if (sqe == nullptr) {
        break;
      }

      io_uring_prep_renameat(sqe, AT_FDCWD, source_paths[submitted], AT_FDCWD,
                             dest_paths[submitted], 0);
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

auto bulk_rename(ring& ring_instance, std::span<const std::string> source_paths,
                 std::span<const std::string> dest_paths) -> bulk_result {
  std::vector<const char*> c_sources;
  std::vector<const char*> c_dests;
  c_sources.reserve(source_paths.size());
  c_dests.reserve(dest_paths.size());
  for (auto const& path : source_paths) {
    c_sources.push_back(path.c_str());
  }
  for (auto const& path : dest_paths) {
    c_dests.push_back(path.c_str());
  }
  return bulk_rename(ring_instance, std::span<const char* const>(c_sources),
                     std::span<const char* const>(c_dests));
}

// ============================================================================
// Bulk symlink
// ============================================================================

auto bulk_symlink(ring& /*ring_instance*/, std::span<const char* const> targets,
                  std::span<const char* const> linkpaths) -> bulk_result {
  if (targets.empty() || targets.size() != linkpaths.size()) {
    return bulk_result{};
  }

  bulk_context context(256);
  bulk_result result;

  std::size_t submitted = 0;
  std::size_t completed = 0;
  std::size_t const total = targets.size();

  while (completed < total) {
    while (submitted < total) {
      struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
      if (sqe == nullptr) {
        break;
      }

      io_uring_prep_symlinkat(sqe, targets[submitted], AT_FDCWD, linkpaths[submitted]);
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

auto bulk_symlink(ring& ring_instance, std::span<const std::string> targets,
                  std::span<const std::string> linkpaths) -> bulk_result {
  std::vector<const char*> c_targets;
  std::vector<const char*> c_linkpaths;
  c_targets.reserve(targets.size());
  c_linkpaths.reserve(linkpaths.size());
  for (auto const& path : targets) {
    c_targets.push_back(path.c_str());
  }
  for (auto const& path : linkpaths) {
    c_linkpaths.push_back(path.c_str());
  }
  return bulk_symlink(ring_instance, std::span<const char* const>(c_targets),
                      std::span<const char* const>(c_linkpaths));
}

// ============================================================================
// Bulk link (hard links)
// ============================================================================

auto bulk_link(ring& /*ring_instance*/, std::span<const char* const> source_paths,
               std::span<const char* const> dest_paths) -> bulk_result {
  if (source_paths.empty() || source_paths.size() != dest_paths.size()) {
    return bulk_result{};
  }

  bulk_context context(256);
  bulk_result result;

  std::size_t submitted = 0;
  std::size_t completed = 0;
  std::size_t const total = source_paths.size();

  while (completed < total) {
    while (submitted < total) {
      struct io_uring_sqe* sqe = io_uring_get_sqe(&context.ring);
      if (sqe == nullptr) {
        break;
      }

      io_uring_prep_linkat(sqe, AT_FDCWD, source_paths[submitted], AT_FDCWD, dest_paths[submitted],
                           0);
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

auto bulk_link(ring& ring_instance, std::span<const std::string> source_paths,
               std::span<const std::string> dest_paths) -> bulk_result {
  std::vector<const char*> c_sources;
  std::vector<const char*> c_dests;
  c_sources.reserve(source_paths.size());
  c_dests.reserve(dest_paths.size());
  for (auto const& path : source_paths) {
    c_sources.push_back(path.c_str());
  }
  for (auto const& path : dest_paths) {
    c_dests.push_back(path.c_str());
  }
  return bulk_link(ring_instance, std::span<const char* const>(c_sources),
                   std::span<const char* const>(c_dests));
}

// ============================================================================
// Bulk readlink
// ============================================================================

auto bulk_readlink(ring& /*ring_instance*/, std::span<const char* const> linkpaths,
                   std::span<std::string> targets) -> bulk_result {
  if (linkpaths.empty() || linkpaths.size() != targets.size()) {
    return bulk_result{};
  }

  // io_uring doesn't have native readlink support
  // Fall back to synchronous readlink for now
  // TODO: consider using io_uring_prep_read on /proc/self/fd/N after openat(O_PATH)
  bulk_result result;

  for (std::size_t index = 0; index < linkpaths.size(); ++index) {
    char buffer[PATH_MAX];
    ssize_t len = readlink(linkpaths[index], buffer, sizeof(buffer) - 1);
    if (len >= 0) {
      buffer[len] = '\0';
      targets[index] = buffer;
      ++result.succeeded;
    } else {
      targets[index].clear();
      ++result.failed;
      result.errors.push_back(errno);
    }
  }

  return result;
}

auto bulk_readlink(ring& ring_instance, std::span<const std::string> linkpaths,
                   std::span<std::string> targets) -> bulk_result {
  std::vector<const char*> c_linkpaths;
  c_linkpaths.reserve(linkpaths.size());
  for (auto const& path : linkpaths) {
    c_linkpaths.push_back(path.c_str());
  }
  return bulk_readlink(ring_instance, std::span<const char* const>(c_linkpaths), targets);
}

// ============================================================================
// Recursive directory copy
// ============================================================================

namespace {

/// entry type for tree walking
enum class entry_type { directory, regular_file, symlink, other };

/// entry info collected during tree walk
struct tree_entry {
  std::string relative_path; // path relative to source root
  entry_type type;
  mode_t mode;
  std::string symlink_target; // only for symlinks
};

/// recursively walk a directory tree and collect all entries
/// returns entries in topological order (directories before their contents)
auto walk_tree(const char* root, bool dereference_symlinks) -> std::vector<tree_entry> {
  std::vector<tree_entry> entries;

  // stack for iterative DFS: (dir_path, relative_prefix)
  std::vector<std::pair<std::string, std::string>> stack;
  stack.emplace_back(root, "");

  while (!stack.empty()) {
    auto [dir_path, relative_prefix] = std::move(stack.back());
    stack.pop_back();

    DIR* dir = opendir(dir_path.c_str());
    if (dir == nullptr) {
      continue; // skip unreadable directories
    }

    // collect entries in this directory first, then process
    // this ensures we add the directory entry before its contents
    std::vector<std::pair<std::string, std::string>> subdirs;

    struct dirent* dirent_entry;
    while ((dirent_entry = readdir(dir)) != nullptr) {
      // skip . and ..
      if (std::strcmp(dirent_entry->d_name, ".") == 0 ||
          std::strcmp(dirent_entry->d_name, "..") == 0) {
        continue;
      }

      std::string name = dirent_entry->d_name;
      std::string full_path = dir_path + "/" + name;
      std::string relative_path = relative_prefix.empty() ? name : relative_prefix + "/" + name;

      struct stat stat_buffer;
      int stat_result;
      if (dereference_symlinks) {
        stat_result = stat(full_path.c_str(), &stat_buffer);
      } else {
        stat_result = lstat(full_path.c_str(), &stat_buffer);
      }

      if (stat_result < 0) {
        continue; // skip unstat-able entries
      }

      tree_entry entry;
      entry.relative_path = relative_path;
      entry.mode = stat_buffer.st_mode & 07777; // permission bits only

      if (S_ISDIR(stat_buffer.st_mode)) {
        entry.type = entry_type::directory;
        entries.push_back(std::move(entry));
        subdirs.emplace_back(full_path, relative_path);
      } else if (S_ISLNK(stat_buffer.st_mode)) {
        entry.type = entry_type::symlink;
        // read symlink target
        char target_buffer[PATH_MAX];
        ssize_t len = readlink(full_path.c_str(), target_buffer, sizeof(target_buffer) - 1);
        if (len >= 0) {
          target_buffer[len] = '\0';
          entry.symlink_target = target_buffer;
          entries.push_back(std::move(entry));
        }
      } else if (S_ISREG(stat_buffer.st_mode)) {
        entry.type = entry_type::regular_file;
        entries.push_back(std::move(entry));
      }
      // skip other types (devices, sockets, etc.)
    }

    closedir(dir);

    // add subdirectories to stack in reverse order for DFS
    for (auto iter = subdirs.rbegin(); iter != subdirs.rend(); ++iter) {
      stack.push_back(std::move(*iter));
    }
  }

  return entries;
}

} // anonymous namespace

auto copy_tree(ring& ring_instance, const char* source, const char* dest,
               copy_tree_options const& options) -> copy_tree_result {
  copy_tree_result result;

  // verify source exists and is a directory
  struct stat source_stat;
  if (stat(source, &source_stat) < 0) {
    result.failed = 1;
    result.errors.emplace_back(source, errno);
    return result;
  }
  if (!S_ISDIR(source_stat.st_mode)) {
    result.failed = 1;
    result.errors.emplace_back(source, ENOTDIR);
    return result;
  }

  // create destination root directory
  if (mkdir(dest, source_stat.st_mode & 07777) < 0 && errno != EEXIST) {
    result.failed = 1;
    result.errors.emplace_back(dest, errno);
    return result;
  }

  // walk the source tree
  std::vector<tree_entry> entries = walk_tree(source, options.dereference_symlinks);

  // separate entries by type
  std::vector<std::string> dir_paths;
  std::vector<mode_t> dir_modes;
  std::vector<std::string> file_sources;
  std::vector<std::string> file_dests;
  std::vector<mode_t> file_modes;
  std::vector<std::string> symlink_targets;
  std::vector<std::string> symlink_paths;

  std::string source_prefix = source;
  std::string dest_prefix = dest;

  for (auto const& entry : entries) {
    std::string dest_path = dest_prefix + "/" + entry.relative_path;

    switch (entry.type) {
      case entry_type::directory:
        dir_paths.push_back(dest_path);
        dir_modes.push_back(entry.mode);
        break;
      case entry_type::regular_file:
        file_sources.push_back(source_prefix + "/" + entry.relative_path);
        file_dests.push_back(dest_path);
        file_modes.push_back(entry.mode);
        break;
      case entry_type::symlink:
        symlink_targets.push_back(entry.symlink_target);
        symlink_paths.push_back(dest_path);
        break;
      case entry_type::other:
        break;
    }
  }

  // phase 1: create all directories (in order - parents before children)
  for (std::size_t index = 0; index < dir_paths.size(); ++index) {
    mode_t mode = options.preserve_permissions ? dir_modes[index] : 0755;
    if (mkdir(dir_paths[index].c_str(), mode) < 0 && errno != EEXIST) {
      ++result.failed;
      result.errors.emplace_back(dir_paths[index], errno);
    } else {
      ++result.directories_created;
    }
  }

  // phase 2: copy all files
  copy_options file_copy_options;
  file_copy_options.buffer_size = options.buffer_size;
  file_copy_options.ring_depth = options.ring_depth;

  for (std::size_t index = 0; index < file_sources.size(); ++index) {
    auto file_result =
        copy_file(ring_instance, file_sources[index], file_dests[index], file_copy_options);
    if (file_result.succeeded > 0) {
      ++result.files_copied;
      // get file size for bytes_copied
      struct stat file_stat;
      if (stat(file_dests[index].c_str(), &file_stat) == 0) {
        result.bytes_copied += static_cast<std::size_t>(file_stat.st_size);
      }
      // set permissions if requested
      if (options.preserve_permissions) {
        chmod(file_dests[index].c_str(), file_modes[index]);
      }
      if (options.on_progress) {
        options.on_progress(result.bytes_copied);
      }
    } else {
      ++result.failed;
      int err = file_result.errors.empty() ? EIO : file_result.errors[0];
      result.errors.emplace_back(file_sources[index], err);
    }
  }

  // phase 3: create all symlinks
  if (!symlink_targets.empty()) {
    auto symlink_result = bulk_symlink(ring_instance, symlink_targets, symlink_paths);
    result.symlinks_created = symlink_result.succeeded;
    result.failed += symlink_result.failed;
    for (std::size_t index = 0; index < symlink_result.errors.size(); ++index) {
      // find which symlink failed - errors are in order
      std::size_t error_index = result.symlinks_created + index;
      if (error_index < symlink_paths.size()) {
        result.errors.emplace_back(symlink_paths[error_index], symlink_result.errors[index]);
      }
    }
  }

  return result;
}

auto copy_tree(ring& ring_instance, std::string const& source, std::string const& dest,
               copy_tree_options const& options) -> copy_tree_result {
  return copy_tree(ring_instance, source.c_str(), dest.c_str(), options);
}

} // namespace evring
