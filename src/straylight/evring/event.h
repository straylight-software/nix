#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include "straylight/evring/handle.h"

namespace evring {

/// operation type for submissions and completions
enum class operation_type : std::uint8_t {
  nop,
  // file operations
  open,
  openat,
  close,
  read,
  write,
  fsync,
  fdatasync,
  // metadata operations
  statx,
  fstat,
  // directory operations
  mkdir,
  mkdirat,
  rmdir,
  unlink,
  unlinkat,
  rename,
  renameat,
  symlink,
  symlinkat,
  link,
  linkat,
  readlink,
  // socket operations
  socket, // create a socket
  connect,
  accept,
  send,
  recv,
  shutdown, // shutdown socket
  poll_add, // poll for readability/writability
  // timing
  timeout,
  cancel,
};

/// an event: what happened (completion from the kernel)
struct event {
  handle resource_handle;
  operation_type operation;
  std::int64_t result; // bytes transferred, fd for open/accept, or -errno

  // for reads: the data that was read (view into caller's buffer)
  std::span<const std::byte> data;

  // for statx: pointer to statx buffer (caller-owned)
  struct statx* statx_buffer;

  // user-provided context for correlation
  std::uint64_t user_data;

  [[nodiscard]] auto ok() const noexcept -> bool { return result >= 0; }

  [[nodiscard]] auto error_code() const noexcept -> int {
    return result < 0 ? static_cast<int>(-result) : 0;
  }
};

/// buffer: either owned or borrowed
struct buffer {
  std::variant<std::vector<std::byte>, std::span<std::byte>> storage_;

  buffer() : storage_(std::vector<std::byte>{}) {}
  explicit buffer(std::size_t byte_count) : storage_(std::vector<std::byte>(byte_count)) {}
  explicit buffer(std::span<std::byte> borrowed) : storage_(borrowed) {}
  explicit buffer(std::vector<std::byte> owned) : storage_(std::move(owned)) {}

  [[nodiscard]] auto span() noexcept -> std::span<std::byte> {
    return std::visit(
        [](auto& storage) -> std::span<std::byte> {
          using storage_type = std::decay_t<decltype(storage)>;
          if constexpr (std::is_same_v<storage_type, std::vector<std::byte>>) {
            return std::span<std::byte>{storage.data(), storage.size()};
          } else {
            return storage;
          }
        },
        storage_);
  }

  [[nodiscard]] auto span() const noexcept -> std::span<const std::byte> {
    return std::visit(
        [](auto const& storage) -> std::span<const std::byte> {
          using storage_type = std::decay_t<decltype(storage)>;
          if constexpr (std::is_same_v<storage_type, std::vector<std::byte>>) {
            return std::span<const std::byte>{storage.data(), storage.size()};
          } else {
            return storage;
          }
        },
        storage_);
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return span().size(); }
  [[nodiscard]] auto data() noexcept -> std::byte* { return span().data(); }
  [[nodiscard]] auto data() const noexcept -> std::byte const* { return span().data(); }
};

/// const buffer for writes
struct const_buffer {
  std::variant<std::vector<std::byte>, std::span<const std::byte>> storage_;

  const_buffer() : storage_(std::vector<std::byte>{}) {}
  explicit const_buffer(std::span<const std::byte> borrowed) : storage_(borrowed) {}
  explicit const_buffer(std::vector<std::byte> owned) : storage_(std::move(owned)) {}

  // convenience: from string
  explicit const_buffer(std::string_view string_data)
      : storage_(std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(string_data.data()),
            reinterpret_cast<const std::byte*>(string_data.data() + string_data.size()))) {}

  [[nodiscard]] auto span() const noexcept -> std::span<const std::byte> {
    return std::visit(
        [](auto const& storage) -> std::span<const std::byte> {
          using storage_type = std::decay_t<decltype(storage)>;
          if constexpr (std::is_same_v<storage_type, std::vector<std::byte>>) {
            return std::span<const std::byte>{storage.data(), storage.size()};
          } else {
            return storage;
          }
        },
        storage_);
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return span().size(); }
  [[nodiscard]] auto data() const noexcept -> std::byte const* { return span().data(); }
};

// ============================================================================
// Operation parameter structs
// ============================================================================

struct open_parameters {
  const char* path;
  int flags;
  mode_t mode;
};

struct openat_parameters {
  int directory_fd;
  const char* path;
  int flags;
  mode_t mode;
};

struct read_parameters {
  std::byte* buffer;
  std::size_t length;
  std::int64_t offset; // -1 for current position
};

struct write_parameters {
  const std::byte* buffer;
  std::size_t length;
  std::int64_t offset; // -1 for current position
};

struct statx_parameters {
  int directory_fd;
  const char* path;
  int flags;
  unsigned int mask;
  struct statx* buffer;
};

struct mkdir_parameters {
  const char* path;
  mode_t mode;
};

struct mkdirat_parameters {
  int directory_fd;
  const char* path;
  mode_t mode;
};

struct unlink_parameters {
  const char* path;
};

struct unlinkat_parameters {
  int directory_fd;
  const char* path;
  int flags; // AT_REMOVEDIR for rmdir
};

struct rename_parameters {
  const char* old_path;
  const char* new_path;
};

struct renameat_parameters {
  int old_directory_fd;
  const char* old_path;
  int new_directory_fd;
  const char* new_path;
  unsigned int flags; // RENAME_NOREPLACE, etc.
};

struct symlink_parameters {
  const char* target;
  const char* linkpath;
};

struct symlinkat_parameters {
  const char* target;
  int directory_fd;
  const char* linkpath;
};

struct link_parameters {
  const char* old_path;
  const char* new_path;
};

struct linkat_parameters {
  int old_directory_fd;
  const char* old_path;
  int new_directory_fd;
  const char* new_path;
  int flags;
};

struct connect_parameters {
  const void* address;
  std::uint32_t address_length;
};

struct accept_parameters {
  void* address;
  std::uint32_t* address_length;
  int flags; // SOCK_NONBLOCK, SOCK_CLOEXEC
};

struct send_parameters {
  const std::byte* buffer;
  std::size_t length;
  int flags; // MSG_DONTWAIT, MSG_NOSIGNAL, etc.
};

struct recv_parameters {
  std::byte* buffer;
  std::size_t length;
  int flags; // MSG_DONTWAIT, MSG_PEEK, etc.
};

struct socket_parameters {
  int domain;   // AF_INET, AF_INET6, AF_UNIX
  int type;     // SOCK_STREAM, SOCK_DGRAM
  int protocol; // 0 for default
  int flags;    // SOCK_NONBLOCK, SOCK_CLOEXEC
};

struct shutdown_parameters {
  int how; // SHUT_RD, SHUT_WR, or SHUT_RDWR
};

struct poll_add_parameters {
  std::uint32_t poll_mask; // POLLIN, POLLOUT, etc.
};

struct timeout_parameters {
  std::uint64_t nanoseconds;
};

struct cancel_parameters {
  handle target;
};

// ============================================================================
// Operation struct with builders
// ============================================================================

struct operation {
  handle resource_handle;
  operation_type type;
  std::uint64_t user_data;

  // operation-specific parameters stored in variant for type safety
  std::variant<std::monostate, open_parameters, openat_parameters, read_parameters,
               write_parameters, statx_parameters, mkdir_parameters, mkdirat_parameters,
               unlink_parameters, unlinkat_parameters, rename_parameters, renameat_parameters,
               symlink_parameters, symlinkat_parameters, link_parameters, linkat_parameters,
               connect_parameters, accept_parameters, send_parameters, recv_parameters,
               socket_parameters, shutdown_parameters, poll_add_parameters, timeout_parameters,
               cancel_parameters>
      parameters;

  // ========================================================================
  // Builders
  // ========================================================================

  static auto make_nop(std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::nop,
        .user_data = user_data,
        .parameters = std::monostate{},
    };
  }

  // --- File operations ---

  static auto make_open(const char* path, int flags, mode_t mode = 0644,
                        std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::open,
        .user_data = user_data,
        .parameters = open_parameters{path, flags, mode},
    };
  }

  static auto make_openat(int directory_fd, const char* path, int flags, mode_t mode = 0644,
                          std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::openat,
        .user_data = user_data,
        .parameters = openat_parameters{directory_fd, path, flags, mode},
    };
  }

  static auto make_close(handle resource, std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = resource,
        .type = operation_type::close,
        .user_data = user_data,
        .parameters = std::monostate{},
    };
  }

  static auto make_read(handle resource, std::span<std::byte> read_buffer, std::int64_t offset = -1,
                        std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = resource,
        .type = operation_type::read,
        .user_data = user_data,
        .parameters = read_parameters{read_buffer.data(), read_buffer.size(), offset},
    };
  }

  static auto make_write(handle resource, std::span<const std::byte> write_buffer,
                         std::int64_t offset = -1, std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = resource,
        .type = operation_type::write,
        .user_data = user_data,
        .parameters = write_parameters{write_buffer.data(), write_buffer.size(), offset},
    };
  }

  static auto make_fsync(handle resource, std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = resource,
        .type = operation_type::fsync,
        .user_data = user_data,
        .parameters = std::monostate{},
    };
  }

  static auto make_fdatasync(handle resource, std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = resource,
        .type = operation_type::fdatasync,
        .user_data = user_data,
        .parameters = std::monostate{},
    };
  }

  // --- Metadata operations ---

  static auto make_statx(int directory_fd, const char* path, int flags, unsigned int mask,
                         struct statx* buffer, std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::statx,
        .user_data = user_data,
        .parameters = statx_parameters{directory_fd, path, flags, mask, buffer},
    };
  }

  // --- Directory operations ---

  static auto make_mkdir(const char* path, mode_t mode = 0755, std::uint64_t user_data = 0)
      -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::mkdir,
        .user_data = user_data,
        .parameters = mkdir_parameters{path, mode},
    };
  }

  static auto make_mkdirat(int directory_fd, const char* path, mode_t mode = 0755,
                           std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::mkdirat,
        .user_data = user_data,
        .parameters = mkdirat_parameters{directory_fd, path, mode},
    };
  }

  static auto make_rmdir(const char* path, std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::rmdir,
        .user_data = user_data,
        .parameters = unlink_parameters{path},
    };
  }

  static auto make_unlink(const char* path, std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::unlink,
        .user_data = user_data,
        .parameters = unlink_parameters{path},
    };
  }

  static auto make_unlinkat(int directory_fd, const char* path, int flags = 0,
                            std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::unlinkat,
        .user_data = user_data,
        .parameters = unlinkat_parameters{directory_fd, path, flags},
    };
  }

  static auto make_rename(const char* old_path, const char* new_path, std::uint64_t user_data = 0)
      -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::rename,
        .user_data = user_data,
        .parameters = rename_parameters{old_path, new_path},
    };
  }

  static auto make_renameat(int old_directory_fd, const char* old_path, int new_directory_fd,
                            const char* new_path, unsigned int flags = 0,
                            std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::renameat,
        .user_data = user_data,
        .parameters =
            renameat_parameters{old_directory_fd, old_path, new_directory_fd, new_path, flags},
    };
  }

  static auto make_symlink(const char* target, const char* linkpath, std::uint64_t user_data = 0)
      -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::symlink,
        .user_data = user_data,
        .parameters = symlink_parameters{target, linkpath},
    };
  }

  static auto make_symlinkat(const char* target, int directory_fd, const char* linkpath,
                             std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::symlinkat,
        .user_data = user_data,
        .parameters = symlinkat_parameters{target, directory_fd, linkpath},
    };
  }

  static auto make_link(const char* old_path, const char* new_path, std::uint64_t user_data = 0)
      -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::link,
        .user_data = user_data,
        .parameters = link_parameters{old_path, new_path},
    };
  }

  static auto make_linkat(int old_directory_fd, const char* old_path, int new_directory_fd,
                          const char* new_path, int flags = 0, std::uint64_t user_data = 0)
      -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::linkat,
        .user_data = user_data,
        .parameters =
            linkat_parameters{old_directory_fd, old_path, new_directory_fd, new_path, flags},
    };
  }

  // --- Socket operations ---

  /// Create a socket
  /// @param domain AF_INET, AF_INET6, AF_UNIX
  /// @param type SOCK_STREAM, SOCK_DGRAM
  /// @param protocol 0 for default
  /// @param flags SOCK_NONBLOCK | SOCK_CLOEXEC
  static auto make_socket(int domain, int type, int protocol = 0, int flags = 0,
                          std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::socket,
        .user_data = user_data,
        .parameters = socket_parameters{domain, type, protocol, flags},
    };
  }

  /// Connect to a remote address
  /// @param socket_handle Handle to the socket resource
  /// @param address Pointer to sockaddr structure
  /// @param address_length Size of the sockaddr structure
  static auto make_connect(handle socket_handle, const void* address, std::uint32_t address_length,
                           std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = socket_handle,
        .type = operation_type::connect,
        .user_data = user_data,
        .parameters = connect_parameters{address, address_length},
    };
  }

  /// Accept a connection on a listening socket
  /// @param socket_handle Handle to the listening socket
  /// @param address Optional buffer to receive client address
  /// @param address_length Optional pointer to receive address length
  /// @param flags SOCK_NONBLOCK | SOCK_CLOEXEC for the new socket
  static auto make_accept(handle socket_handle, void* address = nullptr,
                          std::uint32_t* address_length = nullptr, int flags = 0,
                          std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = socket_handle,
        .type = operation_type::accept,
        .user_data = user_data,
        .parameters = accept_parameters{address, address_length, flags},
    };
  }

  /// Send data on a connected socket
  /// @param socket_handle Handle to the socket
  /// @param buffer Data to send
  /// @param flags MSG_DONTWAIT, MSG_NOSIGNAL, etc.
  static auto make_send(handle socket_handle, std::span<const std::byte> buffer, int flags = 0,
                        std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = socket_handle,
        .type = operation_type::send,
        .user_data = user_data,
        .parameters = send_parameters{buffer.data(), buffer.size(), flags},
    };
  }

  /// Receive data from a connected socket
  /// @param socket_handle Handle to the socket
  /// @param buffer Buffer to receive data into
  /// @param flags MSG_DONTWAIT, MSG_PEEK, etc.
  static auto make_recv(handle socket_handle, std::span<std::byte> buffer, int flags = 0,
                        std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = socket_handle,
        .type = operation_type::recv,
        .user_data = user_data,
        .parameters = recv_parameters{buffer.data(), buffer.size(), flags},
    };
  }

  /// Shutdown a socket
  /// @param socket_handle Handle to the socket
  /// @param how SHUT_RD, SHUT_WR, or SHUT_RDWR
  static auto make_shutdown(handle socket_handle, int how, std::uint64_t user_data = 0)
      -> operation {
    return operation{
        .resource_handle = socket_handle,
        .type = operation_type::shutdown,
        .user_data = user_data,
        .parameters = shutdown_parameters{how},
    };
  }

  /// Poll for socket readability/writability
  /// @param socket_handle Handle to the socket
  /// @param poll_mask POLLIN, POLLOUT, or combination
  static auto make_poll_add(handle socket_handle, std::uint32_t poll_mask,
                            std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = socket_handle,
        .type = operation_type::poll_add,
        .user_data = user_data,
        .parameters = poll_add_parameters{poll_mask},
    };
  }

  // --- Timing ---

  static auto make_timeout(std::uint64_t nanoseconds, std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::timeout,
        .user_data = user_data,
        .parameters = timeout_parameters{nanoseconds},
    };
  }

  static auto make_cancel(handle target, std::uint64_t user_data = 0) -> operation {
    return operation{
        .resource_handle = handle::invalid(),
        .type = operation_type::cancel,
        .user_data = user_data,
        .parameters = cancel_parameters{target},
    };
  }
};

} // namespace evring
