#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <linux/stat.h>
#include <sys/stat.h>
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

      case operation_type::connect:
      case operation_type::accept:
      case operation_type::send:
      case operation_type::recv: {
        // TODO: implement socket operations
        throw std::runtime_error("socket operations not yet implemented");
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

        // for successful reads, provide the data span
        if (inflight.type == operation_type::read && cqe->res > 0) {
          completion_event.data =
              std::span<const std::byte>{inflight.read_buffer, static_cast<std::size_t>(cqe->res)};
        }

        // for successful open/openat, register the new fd
        if ((inflight.type == operation_type::open || inflight.type == operation_type::openat) &&
            cqe->res >= 0) {
          handle new_handle = register_file_descriptor(cqe->res, resource_type::file);
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

} // namespace evring
