/// evring benchmark harness
///
/// Benchmarks:
/// 1. Small file creation (many small files, tests syscall overhead)
/// 2. Large file copy (throughput, tests buffer handling)
/// 3. Metadata operations (stat many files, tests batching)
///
/// Compares evring against:
/// - POSIX syscalls (baseline)
/// - GNU cp (for copy)
/// - System commands via fork/exec

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "straylight/evring/evring.h"

namespace fs = std::filesystem;
using clock_type = std::chrono::high_resolution_clock;

// ============================================================================
// Benchmark infrastructure
// ============================================================================

struct benchmark_result {
  std::string name;
  double elapsed_seconds;
  std::uint64_t operations;
  std::uint64_t bytes_transferred;

  [[nodiscard]] auto ops_per_second() const -> double {
    return static_cast<double>(operations) / elapsed_seconds;
  }

  [[nodiscard]] auto mb_per_second() const -> double {
    return static_cast<double>(bytes_transferred) / (1024.0 * 1024.0) / elapsed_seconds;
  }
};

auto print_result(benchmark_result const& result) -> void {
  std::cout << std::left << std::setw(40) << result.name << ": " << std::fixed
            << std::setprecision(3) << result.elapsed_seconds << "s";

  if (result.operations > 0) {
    std::cout << ", " << std::setprecision(0) << result.ops_per_second() << " ops/s";
  }
  if (result.bytes_transferred > 0) {
    std::cout << ", " << std::setprecision(1) << result.mb_per_second() << " MB/s";
  }
  std::cout << "\n";
}

auto measure(std::function<void()> function) -> double {
  auto const start = clock_type::now();
  function();
  auto const end = clock_type::now();
  return std::chrono::duration<double>(end - start).count();
}

// ============================================================================
// Small file creation benchmark
// ============================================================================

/// Machine that creates many small files using batched io_uring operations
struct file_creator_machine {
  enum class phase { create_files, close_handles, done };

  struct state_type {
    phase current_phase = phase::create_files;
    std::vector<evring::handle> open_handles;
    std::size_t files_created = 0;
    std::size_t files_closed = 0;
    std::size_t next_to_submit = 0;
  };

  std::vector<std::string> paths;
  std::size_t const batch_size = 64;

  explicit file_creator_machine(std::vector<std::string> file_paths)
      : paths(std::move(file_paths)) {}

  [[nodiscard]] auto initial() const -> state_type { return state_type{}; }

  auto step(state_type state, evring::event const& completion_event)
      -> evring::step_result<state_type> {
    std::vector<evring::operation> operations;

    // initial call (empty event) - start creating files
    if (completion_event.operation == evring::operation_type::nop &&
        state.current_phase == phase::create_files && state.next_to_submit == 0) {
      std::size_t const count = std::min(batch_size, paths.size());
      for (std::size_t index = 0; index < count; ++index) {
        operations.push_back(evring::operation::make_open(
            paths[index].c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644, index));
      }
      state.next_to_submit = count;
      return {std::move(state), std::move(operations)};
    }

    switch (state.current_phase) {
      case phase::create_files: {
        if (completion_event.ok()) {
          ++state.files_created;
          state.open_handles.push_back(completion_event.resource_handle);
        }

        if (state.next_to_submit < paths.size()) {
          operations.push_back(evring::operation::make_open(paths[state.next_to_submit].c_str(),
                                                            O_CREAT | O_WRONLY | O_TRUNC, 0644,
                                                            state.next_to_submit));
          ++state.next_to_submit;
        } else if (state.files_created >= paths.size()) {
          // All files created, start closing
          state.current_phase = phase::close_handles;
          for (auto handle : state.open_handles) {
            operations.push_back(evring::operation::make_close(handle));
          }
        }
        break;
      }

      case phase::close_handles: {
        ++state.files_closed;
        if (state.files_closed >= state.open_handles.size()) {
          state.current_phase = phase::done;
        }
        break;
      }

      case phase::done:
        break;
    }

    return {std::move(state), std::move(operations)};
  }

  [[nodiscard]] auto done(state_type const& state) const -> bool {
    return state.current_phase == phase::done;
  }
};

auto bench_create_files_evring(std::string const& directory, std::size_t count)
    -> benchmark_result {
  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    paths.push_back(directory + "/file_" + std::to_string(index));
  }

  file_creator_machine machine(paths);
  auto ring = evring::make_io_uring_ring(256);

  double const elapsed = measure([&] { evring::run(machine, *ring); });

  return benchmark_result{
      .name = "evring: create " + std::to_string(count) + " files",
      .elapsed_seconds = elapsed,
      .operations = count,
      .bytes_transferred = 0,
  };
}

auto bench_create_files_posix(std::string const& directory, std::size_t count) -> benchmark_result {
  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    paths.push_back(directory + "/file_" + std::to_string(index));
  }

  double const elapsed = measure([&] {
    for (auto const& path : paths) {
      int const file_descriptor = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
      if (file_descriptor >= 0) {
        close(file_descriptor);
      }
    }
  });

  return benchmark_result{
      .name = "posix: create " + std::to_string(count) + " files",
      .elapsed_seconds = elapsed,
      .operations = count,
      .bytes_transferred = 0,
  };
}

// ============================================================================
// Large file copy benchmark
// ============================================================================

/// Machine that copies a file using io_uring read/write
struct file_copier_machine {
  enum class phase { open_source, open_dest, copy, close_dest, close_source, done };

  struct state_type {
    phase current_phase = phase::open_source;
    evring::handle source_handle;
    evring::handle dest_handle;
    std::int64_t offset = 0;
    std::uint64_t bytes_copied = 0;
    std::int64_t pending_write_bytes = 0;
  };

  std::string source_path;
  std::string dest_path;
  mutable std::vector<std::byte> buffer; // mutable for const initial()

  static constexpr std::size_t buffer_size = 1024 * 1024; // 1 MB

  file_copier_machine(std::string source, std::string dest)
      : source_path(std::move(source)), dest_path(std::move(dest)), buffer(buffer_size) {}

  [[nodiscard]] auto initial() const -> state_type { return state_type{}; }

  auto step(state_type state, evring::event const& completion_event)
      -> evring::step_result<state_type> {
    std::vector<evring::operation> operations;

    // initial call
    if (completion_event.operation == evring::operation_type::nop &&
        state.current_phase == phase::open_source) {
      operations.push_back(evring::operation::make_open(source_path.c_str(), O_RDONLY));
      return {std::move(state), std::move(operations)};
    }

    switch (state.current_phase) {
      case phase::open_source: {
        if (!completion_event.ok()) {
          state.current_phase = phase::done;
          break;
        }
        state.source_handle = completion_event.resource_handle;
        state.current_phase = phase::open_dest;
        operations.push_back(
            evring::operation::make_open(dest_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644));
        break;
      }

      case phase::open_dest: {
        if (!completion_event.ok()) {
          state.current_phase = phase::close_source;
          operations.push_back(evring::operation::make_close(state.source_handle));
          break;
        }
        state.dest_handle = completion_event.resource_handle;
        state.current_phase = phase::copy;
        operations.push_back(evring::operation::make_read(
            state.source_handle,
            evring::make_stable_span(std::span<std::byte>(buffer.data(), buffer.size())),
            state.offset, 1));
        break;
      }

      case phase::copy: {
        if (completion_event.user_data == 1) {
          // Read completed
          if (completion_event.result <= 0) {
            // EOF or error
            state.current_phase = phase::close_dest;
            operations.push_back(evring::operation::make_close(state.dest_handle));
          } else {
            // Write the data
            state.pending_write_bytes = completion_event.result;
            operations.push_back(evring::operation::make_write(
                state.dest_handle,
                std::span<const std::byte>(buffer.data(),
                                           static_cast<std::size_t>(completion_event.result)),
                state.offset, 2));
          }
        } else if (completion_event.user_data == 2) {
          // Write completed
          if (completion_event.ok()) {
            state.bytes_copied += static_cast<std::uint64_t>(completion_event.result);
            state.offset += state.pending_write_bytes;
            // Read more
            operations.push_back(evring::operation::make_read(
                state.source_handle,
                evring::make_stable_span(std::span<std::byte>(buffer.data(), buffer.size())),
                state.offset, 1));
          } else {
            state.current_phase = phase::close_dest;
            operations.push_back(evring::operation::make_close(state.dest_handle));
          }
        }
        break;
      }

      case phase::close_dest: {
        state.current_phase = phase::close_source;
        operations.push_back(evring::operation::make_close(state.source_handle));
        break;
      }

      case phase::close_source: {
        state.current_phase = phase::done;
        break;
      }

      case phase::done:
        break;
    }

    return {std::move(state), std::move(operations)};
  }

  [[nodiscard]] auto done(state_type const& state) const -> bool {
    return state.current_phase == phase::done;
  }
};

auto bench_copy_file_evring(std::string const& source, std::string const& dest)
    -> benchmark_result {
  auto const file_size = fs::file_size(source);

  file_copier_machine machine(source, dest);
  auto ring = evring::make_io_uring_ring(32);

  double const elapsed = measure([&] { evring::run(machine, *ring); });

  return benchmark_result{
      .name = "evring: copy file (" + std::to_string(file_size / (1024 * 1024)) + " MB)",
      .elapsed_seconds = elapsed,
      .operations = 1,
      .bytes_transferred = file_size,
  };
}

auto bench_copy_file_posix(std::string const& source, std::string const& dest) -> benchmark_result {
  auto const file_size = fs::file_size(source);

  double const elapsed = measure([&] {
    int const source_fd = open(source.c_str(), O_RDONLY);
    int const dest_fd = open(dest.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);

    if (source_fd < 0 || dest_fd < 0) {
      if (source_fd >= 0) {
        close(source_fd);
      }
      if (dest_fd >= 0) {
        close(dest_fd);
      }
      return;
    }

    std::vector<char> buffer(1024 * 1024);
    ssize_t bytes_read;
    while ((bytes_read = read(source_fd, buffer.data(), buffer.size())) > 0) {
      ssize_t total_written = 0;
      while (total_written < bytes_read) {
        ssize_t written = write(dest_fd, buffer.data() + total_written,
                                static_cast<std::size_t>(bytes_read - total_written));
        if (written < 0) {
          break;
        }
        total_written += written;
      }
    }

    close(source_fd);
    close(dest_fd);
  });

  return benchmark_result{
      .name = "posix: copy file (" + std::to_string(file_size / (1024 * 1024)) + " MB)",
      .elapsed_seconds = elapsed,
      .operations = 1,
      .bytes_transferred = file_size,
  };
}

auto bench_copy_file_cp(std::string const& source, std::string const& dest) -> benchmark_result {
  auto const file_size = fs::file_size(source);

  double const elapsed = measure([&] {
    pid_t const pid = fork();
    if (pid == 0) {
      execlp("cp", "cp", source.c_str(), dest.c_str(), nullptr);
      _exit(1);
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
    }
  });

  return benchmark_result{
      .name = "cp: copy file (" + std::to_string(file_size / (1024 * 1024)) + " MB)",
      .elapsed_seconds = elapsed,
      .operations = 1,
      .bytes_transferred = file_size,
  };
}

// ============================================================================
// Metadata operations benchmark (stat many files)
// ============================================================================

/// Machine that stats many files using batched io_uring statx
struct file_statter_machine {
  struct state_type {
    std::size_t completed = 0;
    std::size_t next_to_submit = 0;
  };

  std::vector<std::string> paths;
  mutable std::vector<struct statx> stat_buffers;
  std::size_t const batch_size = 128;

  explicit file_statter_machine(std::vector<std::string> file_paths)
      : paths(std::move(file_paths)), stat_buffers(paths.size()) {}

  [[nodiscard]] auto initial() const -> state_type { return state_type{}; }

  auto step(state_type state, evring::event const& completion_event)
      -> evring::step_result<state_type> {
    std::vector<evring::operation> operations;

    // initial call
    if (completion_event.operation == evring::operation_type::nop && state.next_to_submit == 0) {
      std::size_t const count = std::min(batch_size, paths.size());
      for (std::size_t index = 0; index < count; ++index) {
        operations.push_back(
            evring::operation::make_statx(AT_FDCWD, paths[index].c_str(), 0, STATX_BASIC_STATS,
                                          evring::make_stable_ref(stat_buffers[index]), index));
      }
      state.next_to_submit = count;
      return {std::move(state), std::move(operations)};
    }

    ++state.completed;

    if (state.next_to_submit < paths.size()) {
      operations.push_back(evring::operation::make_statx(
          AT_FDCWD, paths[state.next_to_submit].c_str(), 0, STATX_BASIC_STATS,
          evring::make_stable_ref(stat_buffers[state.next_to_submit]), state.next_to_submit));
      ++state.next_to_submit;
    }

    return {std::move(state), std::move(operations)};
  }

  [[nodiscard]] auto done(state_type const& state) const -> bool {
    return state.completed >= paths.size();
  }
};

auto bench_stat_files_evring(std::vector<std::string> const& paths) -> benchmark_result {
  file_statter_machine machine(paths);
  auto ring = evring::make_io_uring_ring(256);

  double const elapsed = measure([&] { evring::run(machine, *ring); });

  return benchmark_result{
      .name = "evring: stat " + std::to_string(paths.size()) + " files",
      .elapsed_seconds = elapsed,
      .operations = paths.size(),
      .bytes_transferred = 0,
  };
}

auto bench_stat_files_posix(std::vector<std::string> const& paths) -> benchmark_result {
  double const elapsed = measure([&] {
    for (auto const& path : paths) {
      struct stat stat_buffer;
      stat(path.c_str(), &stat_buffer);
    }
  });

  return benchmark_result{
      .name = "posix: stat " + std::to_string(paths.size()) + " files",
      .elapsed_seconds = elapsed,
      .operations = paths.size(),
      .bytes_transferred = 0,
  };
}

// ============================================================================
// Bulk API benchmarks (optimized, bypass state machine)
// ============================================================================

auto bench_create_files_bulk(std::string const& directory, std::size_t count) -> benchmark_result {
  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    paths.push_back(directory + "/file_" + std::to_string(index));
  }

  auto ring = evring::make_io_uring_ring(256);

  double const elapsed = measure([&] { evring::bulk_create_files(*ring, paths); });

  return benchmark_result{
      .name = "bulk: create " + std::to_string(count) + " files",
      .elapsed_seconds = elapsed,
      .operations = count,
      .bytes_transferred = 0,
  };
}

auto bench_stat_files_bulk(std::vector<std::string> const& paths) -> benchmark_result {
  std::vector<struct statx> statx_buffers(paths.size());
  auto ring = evring::make_io_uring_ring(256);

  double const elapsed =
      measure([&] { evring::bulk_stat(*ring, paths, std::span{statx_buffers}); });

  return benchmark_result{
      .name = "bulk: stat " + std::to_string(paths.size()) + " files",
      .elapsed_seconds = elapsed,
      .operations = paths.size(),
      .bytes_transferred = 0,
  };
}

auto bench_copy_file_bulk(std::string const& source, std::string const& dest) -> benchmark_result {
  auto const file_size = fs::file_size(source);
  auto ring = evring::make_io_uring_ring(64);

  evring::copy_options options;
  options.buffer_size = 1024UL * 1024UL; // 1 MB buffers
  options.ring_depth = 32;               // 32 concurrent operations

  double const elapsed = measure([&] { evring::copy_file(*ring, source, dest, options); });

  return benchmark_result{
      .name = "bulk: copy file (" + std::to_string(file_size / (1024 * 1024)) + " MB)",
      .elapsed_seconds = elapsed,
      .operations = 1,
      .bytes_transferred = file_size,
  };
}

// ============================================================================
// SQPOLL benchmarks (kernel-side polling for lower latency)
// ============================================================================

auto bench_create_files_bulk_sqpoll(std::string const& directory, std::size_t count)
    -> benchmark_result {
  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    paths.push_back(directory + "/file_" + std::to_string(index));
  }

  // SQPOLL ring - requires CAP_SYS_NICE or root
  try {
    evring::sqpoll_config sqpoll_cfg;
    sqpoll_cfg.idle_milliseconds = 2000;
    auto ring = evring::make_io_uring_ring(256, evring::ring_flags::sqpoll, sqpoll_cfg);

    double const elapsed = measure([&] { evring::bulk_create_files(*ring, paths); });

    return benchmark_result{
        .name = "bulk+sqpoll: create " + std::to_string(count) + " files",
        .elapsed_seconds = elapsed,
        .operations = count,
        .bytes_transferred = 0,
    };
  } catch (std::runtime_error const&) {
    return benchmark_result{
        .name = "bulk+sqpoll: create " + std::to_string(count) + " files (SKIPPED)",
        .elapsed_seconds = 0,
        .operations = 0,
        .bytes_transferred = 0,
    };
  }
}

auto bench_stat_files_bulk_sqpoll(std::vector<std::string> const& paths) -> benchmark_result {
  std::vector<struct statx> statx_buffers(paths.size());

  try {
    evring::sqpoll_config sqpoll_cfg;
    sqpoll_cfg.idle_milliseconds = 2000;
    auto ring = evring::make_io_uring_ring(256, evring::ring_flags::sqpoll, sqpoll_cfg);

    double const elapsed =
        measure([&] { evring::bulk_stat(*ring, paths, std::span{statx_buffers}); });

    return benchmark_result{
        .name = "bulk+sqpoll: stat " + std::to_string(paths.size()) + " files",
        .elapsed_seconds = elapsed,
        .operations = paths.size(),
        .bytes_transferred = 0,
    };
  } catch (std::runtime_error const&) {
    return benchmark_result{
        .name = "bulk+sqpoll: stat " + std::to_string(paths.size()) + " files (SKIPPED)",
        .elapsed_seconds = 0,
        .operations = 0,
        .bytes_transferred = 0,
    };
  }
}

auto bench_copy_file_bulk_sqpoll(std::string const& source, std::string const& dest)
    -> benchmark_result {
  auto const file_size = fs::file_size(source);

  try {
    evring::sqpoll_config sqpoll_cfg;
    sqpoll_cfg.idle_milliseconds = 2000;
    auto ring = evring::make_io_uring_ring(64, evring::ring_flags::sqpoll, sqpoll_cfg);

    evring::copy_options options;
    options.buffer_size = 1024UL * 1024UL;
    options.ring_depth = 32;

    double const elapsed = measure([&] { evring::copy_file(*ring, source, dest, options); });

    return benchmark_result{
        .name = "bulk+sqpoll: copy file (" + std::to_string(file_size / (1024 * 1024)) + " MB)",
        .elapsed_seconds = elapsed,
        .operations = 1,
        .bytes_transferred = file_size,
    };
  } catch (std::runtime_error const&) {
    return benchmark_result{
        .name = "bulk+sqpoll: copy file (SKIPPED)",
        .elapsed_seconds = 0,
        .operations = 0,
        .bytes_transferred = 0,
    };
  }
}

auto bench_copy_tree_bulk(std::string const& source, std::string const& dest) -> benchmark_result {
  try {
    auto ring = evring::make_io_uring_ring(256);

    evring::copy_tree_options options;
    options.buffer_size = 1024UL * 1024UL;
    options.ring_depth = 32;
    options.preserve_permissions = true;

    evring::copy_tree_result result;
    double const elapsed =
        measure([&] { result = evring::copy_tree(*ring, source, dest, options); });

    return benchmark_result{
        .name = "bulk: copy_tree (" + std::to_string(result.files_copied) + " files)",
        .elapsed_seconds = elapsed,
        .operations = result.files_copied + result.directories_created + result.symlinks_created,
        .bytes_transferred = result.bytes_copied,
    };
  } catch (std::runtime_error const& e) {
    std::cerr << "copy_tree failed: " << e.what() << "\n";
    return benchmark_result{
        .name = "bulk: copy_tree (FAILED)",
        .elapsed_seconds = 0,
        .operations = 0,
        .bytes_transferred = 0,
    };
  }
}

auto bench_copy_tree_cp(std::string const& source, std::string const& dest) -> benchmark_result {
  // count files first
  std::size_t file_count = 0;
  std::uint64_t total_bytes = 0;
  for (auto const& entry :
       fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied)) {
    if (entry.is_regular_file()) {
      ++file_count;
      total_bytes += entry.file_size();
    }
  }

  double const elapsed = measure([&] {
    pid_t const pid = fork();
    if (pid == 0) {
      execlp("cp", "cp", "-r", source.c_str(), dest.c_str(), nullptr);
      _exit(1);
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
    }
  });

  return benchmark_result{
      .name = "cp -r: copy_tree (" + std::to_string(file_count) + " files)",
      .elapsed_seconds = elapsed,
      .operations = file_count,
      .bytes_transferred = total_bytes,
  };
}

// ============================================================================
// Setup/teardown utilities
// ============================================================================

auto create_test_file(std::string const& path, std::size_t size_bytes) -> void {
  std::vector<char> buffer(1024 * 1024); // 1 MB chunks
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 255);

  for (auto& byte : buffer) {
    byte = static_cast<char>(dist(rng));
  }

  int const file_descriptor = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (file_descriptor < 0) {
    std::cerr << "Failed to create test file: " << path << "\n";
    return;
  }

  std::size_t written = 0;
  while (written < size_bytes) {
    std::size_t const chunk = std::min(buffer.size(), size_bytes - written);
    ssize_t const result = write(file_descriptor, buffer.data(), chunk);
    if (result < 0) {
      break;
    }
    written += static_cast<std::size_t>(result);
  }

  close(file_descriptor);
}

auto collect_files_in_directory(std::string const& directory, std::size_t max_files = 10000)
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
    // Ignore permission errors, etc.
  }
  return paths;
}

// ============================================================================
// Main
// ============================================================================

auto main(int argc, char** argv) -> int {
  std::string test_directory = "/tmp/evring_bench";

  // Parse arguments
  bool run_create_bench = true;
  bool run_copy_bench = true;
  bool run_stat_bench = true;
  std::size_t num_files = 1000;
  std::size_t copy_size_mb = 100;

  for (int index = 1; index < argc; ++index) {
    std::string_view argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: " << argv[0] << " [options]\n"
                << "Options:\n"
                << "  --dir <path>     Test directory (default: /tmp/evring_bench)\n"
                << "  --files <n>      Number of files to create (default: 1000)\n"
                << "  --copy-size <n>  Size of file to copy in MB (default: 100)\n"
                << "  --no-create      Skip file creation benchmark\n"
                << "  --no-copy        Skip file copy benchmark\n"
                << "  --no-stat        Skip stat benchmark\n";
      return 0;
    }
    if (argument == "--dir" && index + 1 < argc) {
      test_directory = argv[++index];
    } else if (argument == "--files" && index + 1 < argc) {
      num_files = static_cast<std::size_t>(std::stoul(argv[++index]));
    } else if (argument == "--copy-size" && index + 1 < argc) {
      copy_size_mb = static_cast<std::size_t>(std::stoul(argv[++index]));
    } else if (argument == "--no-create") {
      run_create_bench = false;
    } else if (argument == "--no-copy") {
      run_copy_bench = false;
    } else if (argument == "--no-stat") {
      run_stat_bench = false;
    }
  }

  std::cout << "evring benchmark\n";
  std::cout << "================\n";
  std::cout << "test directory: " << test_directory << "\n";
  std::cout << "file count: " << num_files << "\n";
  std::cout << "copy size: " << copy_size_mb << " MB\n\n";

  // Setup
  fs::remove_all(test_directory);
  fs::create_directories(test_directory);

  std::string const create_directory = test_directory + "/create_test";
  std::string const copy_source = test_directory + "/copy_source.bin";
  std::string const copy_dest_evring = test_directory + "/copy_dest_evring.bin";
  std::string const copy_dest_posix = test_directory + "/copy_dest_posix.bin";
  std::string const copy_dest_cp = test_directory + "/copy_dest_cp.bin";

  std::vector<benchmark_result> results;

  // File creation benchmark
  if (run_create_bench) {
    std::cout << "--- File creation benchmark ---\n";

    fs::create_directories(create_directory + "/posix");
    results.push_back(bench_create_files_posix(create_directory + "/posix", num_files));
    print_result(results.back());

    // Skip evring state machine create - too slow for large file counts
    // fs::create_directories(create_directory + "/evring");
    // results.push_back(bench_create_files_evring(create_directory + "/evring", num_files));
    // print_result(results.back());

    fs::create_directories(create_directory + "/bulk");
    results.push_back(bench_create_files_bulk(create_directory + "/bulk", num_files));
    print_result(results.back());

    fs::create_directories(create_directory + "/sqpoll");
    results.push_back(bench_create_files_bulk_sqpoll(create_directory + "/sqpoll", num_files));
    print_result(results.back());

    std::cout << "\n";
  }

  // File copy benchmark
  if (run_copy_bench) {
    std::cout << "--- File copy benchmark ---\n";
    std::cout << "Creating " << copy_size_mb << " MB test file...\n";
    create_test_file(copy_source, copy_size_mb * 1024 * 1024);

    results.push_back(bench_copy_file_posix(copy_source, copy_dest_posix));
    print_result(results.back());
    fs::remove(copy_dest_posix);

    results.push_back(bench_copy_file_cp(copy_source, copy_dest_cp));
    print_result(results.back());
    fs::remove(copy_dest_cp);

    // Skip evring state machine copy - it's sequential and too slow for large files
    // results.push_back(bench_copy_file_evring(copy_source, copy_dest_evring));
    // print_result(results.back());
    // fs::remove(copy_dest_evring);

    std::string const copy_dest_bulk = test_directory + "/copy_dest_bulk.bin";
    results.push_back(bench_copy_file_bulk(copy_source, copy_dest_bulk));
    print_result(results.back());
    fs::remove(copy_dest_bulk);

    std::string const copy_dest_sqpoll = test_directory + "/copy_dest_sqpoll.bin";
    results.push_back(bench_copy_file_bulk_sqpoll(copy_source, copy_dest_sqpoll));
    print_result(results.back());
    fs::remove(copy_dest_sqpoll);

    std::cout << "\n";
  }

  // Stat benchmark
  if (run_stat_bench) {
    std::cout << "--- Stat benchmark ---\n";

    // Use a directory with many files for stat benchmark
    std::string const stat_source = "/nix/store";
    if (!fs::exists(stat_source)) {
      std::cout << "Skipping stat benchmark: " << stat_source << " not found\n";
    } else {
      std::cout << "Collecting files from " << stat_source << "...\n";
      auto stat_paths = collect_files_in_directory(stat_source);
      std::cout << "Found " << stat_paths.size() << " files\n";

      if (!stat_paths.empty()) {
        results.push_back(bench_stat_files_posix(stat_paths));
        print_result(results.back());

        // Skip evring state machine stat - slower than bulk
        // results.push_back(bench_stat_files_evring(stat_paths));
        // print_result(results.back());

        results.push_back(bench_stat_files_bulk(stat_paths));
        print_result(results.back());

        results.push_back(bench_stat_files_bulk_sqpoll(stat_paths));
        print_result(results.back());
      }
    }

    std::cout << "\n";
  }

  // Copy tree benchmark (use the created files from create_directory)
  if (run_create_bench && run_copy_bench) {
    std::cout << "--- Copy tree benchmark ---\n";

    std::string const tree_source = create_directory + "/posix";
    std::string const tree_dest_bulk = test_directory + "/tree_dest_bulk";
    std::string const tree_dest_cp = test_directory + "/tree_dest_cp";

    results.push_back(bench_copy_tree_cp(tree_source, tree_dest_cp));
    print_result(results.back());
    fs::remove_all(tree_dest_cp);

    results.push_back(bench_copy_tree_bulk(tree_source, tree_dest_bulk));
    print_result(results.back());
    fs::remove_all(tree_dest_bulk);

    std::cout << "\n";
  }

  // Summary
  std::cout << "--- Summary ---\n";
  for (auto const& result : results) {
    print_result(result);
  }

  // Cleanup
  std::cout << "\nCleaning up...\n";
  fs::remove_all(test_directory);

  return 0;
}
