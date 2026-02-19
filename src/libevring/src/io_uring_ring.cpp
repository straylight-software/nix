#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <linux/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "evring/ring.h"

namespace evring {

namespace {

/// context stored with each in-flight operation
struct inflight_operation {
  handle resource_handle;
  operation_type type;
  std::uint64_t user_data;
  std::byte* read_buffer;
  std::size_t read_length;
  struct statx* statx_buffer;
};

} // namespace

/// io_uring implementation of ring interface
class io_uring_ring final : public ring {
public:
  explicit io_uring_ring(unsigned entries, unsigned flags) {
    int result = io_uring_queue_init(entries, &ring_, flags);
    if (result < 0) {
      throw std::runtime_error(std::string("io_uring_queue_init failed: ") +
                               std::strerror(-result));
    }
  }

  /// construct with full params (for SQPOLL configuration)
  explicit io_uring_ring(unsigned entries, struct io_uring_params& params) {
    int result = io_uring_queue_init_params(entries, &ring_, &params);
    if (result < 0) {
      throw std::runtime_error(std::string("io_uring_queue_init_params failed: ") +
                               std::strerror(-result));
    }
  }

  ~io_uring_ring() override { io_uring_queue_exit(&ring_); }

  void enqueue(const operation& operation_to_enqueue) override {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      // submission queue is full - need to submit and retry
      io_uring_submit(&ring_);
      sqe = io_uring_get_sqe(&ring_);
      if (sqe == nullptr) {
        throw std::runtime_error("io_uring submission queue full after submit");
      }
    }

    // allocate inflight context
    std::size_t inflight_index = inflight_operations_.size();
    inflight_operations_.push_back(inflight_operation{
        .resource_handle = operation_to_enqueue.resource_handle,
        .type = operation_to_enqueue.type,
        .user_data = operation_to_enqueue.user_data,
        .read_buffer = nullptr,
        .read_length = 0,
        .statx_buffer = nullptr,
    });

    io_uring_sqe_set_data64(sqe, inflight_index);

    switch (operation_to_enqueue.type) {
      case operation_type::nop: {
        io_uring_prep_nop(sqe);
        break;
      }

      case operation_type::open: {
        const auto& parameters = std::get<open_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_openat(sqe, AT_FDCWD, parameters.path, parameters.flags, parameters.mode);
        break;
      }

      case operation_type::openat: {
        const auto& parameters = std::get<openat_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_openat(sqe, parameters.directory_fd, parameters.path, parameters.flags,
                             parameters.mode);
        break;
      }

      case operation_type::close: {
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        io_uring_prep_close(sqe, file_descriptor);
        break;
      }

      case operation_type::read: {
        const auto& parameters = std::get<read_parameters>(operation_to_enqueue.parameters);
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        std::uint64_t offset = parameters.offset >= 0
                                   ? static_cast<std::uint64_t>(parameters.offset)
                                   : static_cast<std::uint64_t>(-1);
        io_uring_prep_read(sqe, file_descriptor, parameters.buffer, parameters.length, offset);
        inflight_operations_[inflight_index].read_buffer = parameters.buffer;
        inflight_operations_[inflight_index].read_length = parameters.length;
        break;
      }

      case operation_type::write: {
        const auto& parameters = std::get<write_parameters>(operation_to_enqueue.parameters);
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        std::uint64_t offset = parameters.offset >= 0
                                   ? static_cast<std::uint64_t>(parameters.offset)
                                   : static_cast<std::uint64_t>(-1);
        io_uring_prep_write(sqe, file_descriptor, const_cast<std::byte*>(parameters.buffer),
                            parameters.length, offset);
        break;
      }

      case operation_type::fsync: {
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        io_uring_prep_fsync(sqe, file_descriptor, 0);
        break;
      }

      case operation_type::fdatasync: {
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        io_uring_prep_fsync(sqe, file_descriptor, IORING_FSYNC_DATASYNC);
        break;
      }

      case operation_type::statx: {
        const auto& parameters = std::get<statx_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_statx(sqe, parameters.directory_fd, parameters.path, parameters.flags,
                            parameters.mask, parameters.buffer);
        inflight_operations_[inflight_index].statx_buffer = parameters.buffer;
        break;
      }

      case operation_type::mkdir: {
        const auto& parameters = std::get<mkdir_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_mkdirat(sqe, AT_FDCWD, parameters.path, parameters.mode);
        break;
      }

      case operation_type::mkdirat: {
        const auto& parameters = std::get<mkdirat_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_mkdirat(sqe, parameters.directory_fd, parameters.path, parameters.mode);
        break;
      }

      case operation_type::rmdir: {
        const auto& parameters = std::get<unlink_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_unlinkat(sqe, AT_FDCWD, parameters.path, AT_REMOVEDIR);
        break;
      }

      case operation_type::unlink: {
        const auto& parameters = std::get<unlink_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_unlinkat(sqe, AT_FDCWD, parameters.path, 0);
        break;
      }

      case operation_type::unlinkat: {
        const auto& parameters = std::get<unlinkat_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_unlinkat(sqe, parameters.directory_fd, parameters.path, parameters.flags);
        break;
      }

      case operation_type::rename: {
        const auto& parameters = std::get<rename_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_renameat(sqe, AT_FDCWD, parameters.old_path, AT_FDCWD, parameters.new_path,
                               0);
        break;
      }

      case operation_type::renameat: {
        const auto& parameters = std::get<renameat_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_renameat(sqe, parameters.old_directory_fd, parameters.old_path,
                               parameters.new_directory_fd, parameters.new_path, parameters.flags);
        break;
      }

      case operation_type::symlink: {
        const auto& parameters = std::get<symlink_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_symlinkat(sqe, parameters.target, AT_FDCWD, parameters.linkpath);
        break;
      }

      case operation_type::symlinkat: {
        const auto& parameters = std::get<symlinkat_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_symlinkat(sqe, parameters.target, parameters.directory_fd,
                                parameters.linkpath);
        break;
      }

      case operation_type::link: {
        const auto& parameters = std::get<link_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_linkat(sqe, AT_FDCWD, parameters.old_path, AT_FDCWD, parameters.new_path, 0);
        break;
      }

      case operation_type::linkat: {
        const auto& parameters = std::get<linkat_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_linkat(sqe, parameters.old_directory_fd, parameters.old_path,
                             parameters.new_directory_fd, parameters.new_path, parameters.flags);
        break;
      }

      case operation_type::fstat: {
        // fstat via statx with empty path and AT_EMPTY_PATH
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        const auto& parameters = std::get<statx_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_statx(sqe, file_descriptor, "", AT_EMPTY_PATH, parameters.mask,
                            parameters.buffer);
        inflight_operations_[inflight_index].statx_buffer = parameters.buffer;
        break;
      }

      case operation_type::readlink: {
        // io_uring doesn't have native readlink - fall back to sync later
        // For now, just prep a nop and we'll handle specially
        io_uring_prep_nop(sqe);
        break;
      }

      case operation_type::timeout: {
        const auto& parameters = std::get<timeout_parameters>(operation_to_enqueue.parameters);
        timeout_specs_.push_back(__kernel_timespec{
            .tv_sec = static_cast<__s64>(parameters.nanoseconds / 1'000'000'000),
            .tv_nsec = static_cast<long long>(parameters.nanoseconds % 1'000'000'000),
        });
        io_uring_prep_timeout(sqe, &timeout_specs_.back(), 0, 0);
        break;
      }

      case operation_type::cancel: {
        const auto& parameters = std::get<cancel_parameters>(operation_to_enqueue.parameters);
        io_uring_prep_cancel64(sqe, parameters.target.index, 0);
        break;
      }

      case operation_type::socket: {
        const auto& parameters = std::get<socket_parameters>(operation_to_enqueue.parameters);
        // SOCK_NONBLOCK and SOCK_CLOEXEC can be ORed into type
        int type_with_flags = parameters.type | parameters.flags;
        io_uring_prep_socket(sqe, parameters.domain, type_with_flags, parameters.protocol, 0);
        break;
      }

      case operation_type::connect: {
        const auto& parameters = std::get<connect_parameters>(operation_to_enqueue.parameters);
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        io_uring_prep_connect(sqe, file_descriptor,
                              static_cast<const struct sockaddr*>(parameters.address),
                              parameters.address_length);
        break;
      }

      case operation_type::accept: {
        const auto& parameters = std::get<accept_parameters>(operation_to_enqueue.parameters);
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        // Use accept with flags for SOCK_NONBLOCK/SOCK_CLOEXEC on the new socket
        io_uring_prep_accept(sqe, file_descriptor,
                             static_cast<struct sockaddr*>(parameters.address),
                             parameters.address_length, parameters.flags);
        break;
      }

      case operation_type::send: {
        const auto& parameters = std::get<send_parameters>(operation_to_enqueue.parameters);
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        io_uring_prep_send(sqe, file_descriptor, parameters.buffer, parameters.length,
                           parameters.flags);
        break;
      }

      case operation_type::recv: {
        const auto& parameters = std::get<recv_parameters>(operation_to_enqueue.parameters);
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        io_uring_prep_recv(sqe, file_descriptor, parameters.buffer, parameters.length,
                           parameters.flags);
        // track buffer for data span in completion
        inflight_operations_[inflight_index].read_buffer = parameters.buffer;
        inflight_operations_[inflight_index].read_length = parameters.length;
        break;
      }

      case operation_type::shutdown: {
        const auto& parameters = std::get<shutdown_parameters>(operation_to_enqueue.parameters);
        int file_descriptor = get_file_descriptor(operation_to_enqueue.resource_handle);
        io_uring_prep_shutdown(sqe, file_descriptor, parameters.how);
        break;
      }
    }

    pending_count_++;
  }

  auto submit_and_wait(int min_completions) -> std::span<event> override {
    if (pending_count_ == 0 && min_completions > 0) {
      completed_events_.clear();
      return completed_events_;
    }

    int submitted = io_uring_submit_and_wait(&ring_, min_completions);
    if (submitted < 0) {
      throw std::runtime_error(std::string("io_uring_submit_and_wait failed: ") +
                               std::strerror(-submitted));
    }

    return harvest_completions();
  }

  auto poll() -> std::span<event> override {
    if (pending_count_ > 0) {
      io_uring_submit(&ring_);
    }
    return harvest_completions();
  }

  auto register_file_descriptor(int file_descriptor, resource_type type) -> handle override {
    return resources_.insert(resource{
        .file_descriptor = file_descriptor,
        .type = type,
        .closing = false,
    });
  }

  [[nodiscard]] auto get_file_descriptor(handle resource_handle) const -> int override {
    const resource* resource_ptr = resources_.get(resource_handle);
    if (resource_ptr == nullptr) {
      return -1;
    }
    return resource_ptr->file_descriptor;
  }

  [[nodiscard]] auto pending() const -> std::size_t override { return pending_count_; }

  [[nodiscard]] auto active_handles() const -> std::size_t override { return resources_.size(); }

  auto submit() -> int override { return io_uring_submit(&ring_); }

  auto harvest() -> std::span<event> override { return harvest_completions(); }

  [[nodiscard]] auto cq_ready() const -> std::size_t override { return io_uring_cq_ready(&ring_); }

  [[nodiscard]] auto sq_space() const -> std::size_t override {
    return io_uring_sq_space_left(&ring_);
  }

  /// get raw io_uring pointer for registration functions
  [[nodiscard]] auto raw_ring() -> struct io_uring* { return &ring_; }

private:
  auto harvest_completions() -> std::span<event> {
    completed_events_.clear();

    struct io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe) {
      std::uint64_t inflight_index = io_uring_cqe_get_data64(cqe);

      if (inflight_index < inflight_operations_.size()) {
        const inflight_operation& inflight = inflight_operations_[inflight_index];

        event completion_event{
            .resource_handle = inflight.resource_handle,
            .operation = inflight.type,
            .result = cqe->res,
            .data = {},
            .statx_buffer = inflight.statx_buffer,
            .user_data = inflight.user_data,
        };

        // for successful reads and recv, provide the data span
        if ((inflight.type == operation_type::read || inflight.type == operation_type::recv) &&
            cqe->res > 0) {
          completion_event.data =
              std::span<const std::byte>{inflight.read_buffer, static_cast<std::size_t>(cqe->res)};
        }

        // for successful open/openat, register the new fd as a file
        if ((inflight.type == operation_type::open || inflight.type == operation_type::openat) &&
            cqe->res >= 0) {
          handle new_handle = register_file_descriptor(cqe->res, resource_type::file);
          completion_event.resource_handle = new_handle;
        }

        // for successful socket creation, register the new fd as a socket
        if (inflight.type == operation_type::socket && cqe->res >= 0) {
          handle new_handle = register_file_descriptor(cqe->res, resource_type::socket);
          completion_event.resource_handle = new_handle;
        }

        // for successful accept, register the new fd as a socket
        if (inflight.type == operation_type::accept && cqe->res >= 0) {
          handle new_handle = register_file_descriptor(cqe->res, resource_type::socket);
          completion_event.resource_handle = new_handle;
        }

        completed_events_.push_back(completion_event);
      }

      count++;
      pending_count_--;
    }

    io_uring_cq_advance(&ring_, count);

    // clear inflight operations when all pending are done
    if (pending_count_ == 0) {
      inflight_operations_.clear();
      timeout_specs_.clear();
    }

    return completed_events_;
  }

  struct io_uring ring_{};
  handle_table<resource> resources_;
  std::vector<inflight_operation> inflight_operations_;
  std::vector<__kernel_timespec> timeout_specs_;
  std::vector<event> completed_events_;
  std::size_t pending_count_{0};
};

auto make_io_uring_ring(unsigned entries, unsigned flags) -> std::unique_ptr<ring> {
  return std::make_unique<io_uring_ring>(entries, flags);
}

// ============================================================================
// Registered files implementation
// ============================================================================

class io_uring_registered_files final : public registered_files {
public:
  io_uring_registered_files(struct io_uring* ring, std::vector<int> initial_slots)
      : ring_(ring), slots_(std::move(initial_slots)), used_count_(0) {
    // count non-empty slots
    for (int fd : slots_) {
      if (fd != -1) {
        ++used_count_;
      }
    }
  }

  ~io_uring_registered_files() override { io_uring_unregister_files(ring_); }

  [[nodiscard]] auto capacity() const -> std::size_t override { return slots_.size(); }

  [[nodiscard]] auto size() const -> std::size_t override { return used_count_; }

  auto add(int fd) -> int override {
    // find first empty slot
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i] == -1) {
        if (update(i, fd)) {
          return static_cast<int>(i);
        }
        return -1;
      }
    }
    return -1; // no slots available
  }

  auto update(std::size_t slot, int fd) -> bool override {
    if (slot >= slots_.size()) {
      return false;
    }

    int result = io_uring_register_files_update(ring_, static_cast<unsigned>(slot), &fd, 1);
    if (result < 0) {
      return false;
    }

    if (slots_[slot] == -1 && fd != -1) {
      ++used_count_;
    } else if (slots_[slot] != -1 && fd == -1) {
      --used_count_;
    }
    slots_[slot] = fd;
    return true;
  }

  auto remove(std::size_t slot) -> bool override { return update(slot, -1); }

  [[nodiscard]] auto get(std::size_t slot) const -> int override {
    if (slot >= slots_.size()) {
      return -1;
    }
    return slots_[slot];
  }

private:
  struct io_uring* ring_;
  std::vector<int> slots_;
  std::size_t used_count_;
};

// ============================================================================
// Registered buffers implementation
// ============================================================================

class io_uring_registered_buffers final : public registered_buffers {
public:
  io_uring_registered_buffers(struct io_uring* ring, std::vector<std::span<std::byte>> bufs)
      : ring_(ring), buffers_(std::move(bufs)) {}

  ~io_uring_registered_buffers() override { io_uring_unregister_buffers(ring_); }

  [[nodiscard]] auto capacity() const -> std::size_t override { return buffers_.size(); }

  [[nodiscard]] auto get(std::size_t slot) const -> std::span<std::byte> override {
    if (slot >= buffers_.size()) {
      return {};
    }
    return buffers_[slot];
  }

  [[nodiscard]] auto buffers() const -> std::span<const std::span<std::byte>> override {
    return buffers_;
  }

private:
  struct io_uring* ring_;
  std::vector<std::span<std::byte>> buffers_;
};

// ============================================================================
// Ring factory functions
// ============================================================================

auto make_io_uring_ring(unsigned entries, ring_flags flags, sqpoll_config const& sqpoll)
    -> std::unique_ptr<ring> {
  unsigned raw_flags = 0;

  if ((flags & ring_flags::sqpoll) != ring_flags::none) {
    raw_flags |= IORING_SETUP_SQPOLL;
  }
  if ((flags & ring_flags::iopoll) != ring_flags::none) {
    raw_flags |= IORING_SETUP_IOPOLL;
  }
  if ((flags & ring_flags::single_issuer) != ring_flags::none) {
    raw_flags |= IORING_SETUP_SINGLE_ISSUER;
  }
  if ((flags & ring_flags::defer_taskrun) != ring_flags::none) {
    raw_flags |= IORING_SETUP_DEFER_TASKRUN;
  }

  // for SQPOLL, we need to use io_uring_queue_init_params to set sq_thread_idle
  if ((flags & ring_flags::sqpoll) != ring_flags::none) {
    struct io_uring_params params{};
    params.flags = raw_flags;
    params.sq_thread_idle = sqpoll.idle_milliseconds;
    if (sqpoll.cpu >= 0) {
      params.flags |= IORING_SETUP_SQ_AFF;
      params.sq_thread_cpu = static_cast<unsigned>(sqpoll.cpu);
    }
    return std::make_unique<io_uring_ring>(entries, params);
  }

  return std::make_unique<io_uring_ring>(entries, raw_flags);
}

auto register_files(ring& ring_instance, std::span<const int> fds)
    -> std::unique_ptr<registered_files> {
  // downcast to io_uring_ring to access raw_ring()
  auto* io_ring = dynamic_cast<io_uring_ring*>(&ring_instance);
  if (io_ring == nullptr) {
    return nullptr;
  }

  // register the files with the kernel
  int result = io_uring_register_files(io_ring->raw_ring(), const_cast<int*>(fds.data()),
                                       static_cast<unsigned>(fds.size()));
  if (result < 0) {
    return nullptr;
  }

  // create the registered_files object with the fds as initial slots
  std::vector<int> slots(fds.begin(), fds.end());
  return std::make_unique<io_uring_registered_files>(io_ring->raw_ring(), std::move(slots));
}

auto register_file_slots(ring& ring_instance, std::size_t num_slots)
    -> std::unique_ptr<registered_files> {
  auto* io_ring = dynamic_cast<io_uring_ring*>(&ring_instance);
  if (io_ring == nullptr) {
    return nullptr;
  }

  // register sparse file table (all -1)
  std::vector<int> slots(num_slots, -1);
  int result =
      io_uring_register_files(io_ring->raw_ring(), slots.data(), static_cast<unsigned>(num_slots));
  if (result < 0) {
    return nullptr;
  }

  return std::make_unique<io_uring_registered_files>(io_ring->raw_ring(), std::move(slots));
}

auto register_buffers(ring& ring_instance, std::span<std::span<std::byte>> bufs)
    -> std::unique_ptr<registered_buffers> {
  auto* io_ring = dynamic_cast<io_uring_ring*>(&ring_instance);
  if (io_ring == nullptr) {
    return nullptr;
  }

  // build iovec array for registration
  std::vector<struct iovec> iovecs;
  iovecs.reserve(bufs.size());
  for (auto const& buf : bufs) {
    iovecs.push_back({
        .iov_base = buf.data(),
        .iov_len = buf.size(),
    });
  }

  int result = io_uring_register_buffers(io_ring->raw_ring(), iovecs.data(),
                                         static_cast<unsigned>(iovecs.size()));
  if (result < 0) {
    return nullptr;
  }

  std::vector<std::span<std::byte>> buffer_spans(bufs.begin(), bufs.end());
  return std::make_unique<io_uring_registered_buffers>(io_ring->raw_ring(),
                                                       std::move(buffer_spans));
}

} // namespace evring
