// straylight::nix::primitives::lock
//
// Portable mutual exclusion for the legacy store.
//
// Properties (to be proven in Lean4):
//   1. Safety: At most one holder at a time
//   2. Liveness: Dead holder → lock released
//   3. No daemon: Pure kernel primitives
//   4. Fork-safe: Child does not inherit lock ownership
//
// Implementation:
//   - flock() for kernel-managed mutual exclusion
//   - PID written to lock file for liveness checking
//   - RAII ensures release on scope exit
//   - O_CLOEXEC prevents leak across exec

#pragma once

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

namespace straylight::nix::sync {

// ============================================================================
// Lock errors
// ============================================================================

enum class lock_error : std::uint8_t {
  open_failed,
  lock_failed,
  io_error,
  interrupted,
};

template <typename T>
using lock_result = std::expected<T, lock_error>;

// ============================================================================
// exclusive_lock - RAII mutual exclusion
// ============================================================================

/// Exclusive lock on a file path.
///
/// Guarantees:
///   - At most one process holds the lock at a time
///   - Lock is released when this object is destroyed
///   - Lock is released if the process dies (kernel cleanup)
///   - Forked children do not inherit ownership
///
/// Usage:
///   {
///       auto lock = exclusive_lock::acquire(path);
///       if (!lock) { /* handle error */ }
///       // Critical section - only one process here
///   }
///   // Lock automatically released
///
class exclusive_lock {
public:
  // Factory method - returns error instead of throwing
  [[nodiscard]] static auto acquire(const std::filesystem::path& path)
      -> lock_result<exclusive_lock>;

  // Try to acquire without blocking
  [[nodiscard]] static auto try_acquire(const std::filesystem::path& path)
      -> lock_result<exclusive_lock>;

  // Destructor releases lock (if we're the owner)
  ~exclusive_lock();

  // Non-copyable
  exclusive_lock(const exclusive_lock&) = delete;
  auto operator=(const exclusive_lock&) -> exclusive_lock& = delete;

  // Movable
  exclusive_lock(exclusive_lock&& other) noexcept;
  auto operator=(exclusive_lock&& other) noexcept -> exclusive_lock&;

  // Check if lock is held
  [[nodiscard]] auto is_held() const noexcept -> bool { return fd_ >= 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return is_held(); }

  // Release early (normally done by destructor)
  void release() noexcept;

  // Get path for diagnostics
  [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }

private:
  // Private constructor - use acquire() factory
  exclusive_lock(int fd, std::filesystem::path path, pid_t pid) noexcept;

  // Write our PID to the lock file (for liveness checking)
  void write_pid() noexcept;

  // Read PID of current holder (if any)
  [[nodiscard]] static auto read_pid(int fd) noexcept -> pid_t;

  // Check if a process is alive
  [[nodiscard]] static auto is_process_alive(pid_t pid) noexcept -> bool;

  int fd_ = -1;
  std::filesystem::path path_;
  pid_t owning_pid_ = 0;
};

// ============================================================================
// shared_lock - Multiple readers, exclusive with writers
// ============================================================================

/// Shared (read) lock on a file path.
///
/// Multiple shared locks can be held simultaneously.
/// Shared locks are exclusive with exclusive_lock.
///
class shared_lock {
public:
  [[nodiscard]] static auto acquire(const std::filesystem::path& path) -> lock_result<shared_lock>;

  [[nodiscard]] static auto try_acquire(const std::filesystem::path& path)
      -> lock_result<shared_lock>;

  ~shared_lock();

  shared_lock(const shared_lock&) = delete;
  auto operator=(const shared_lock&) -> shared_lock& = delete;

  shared_lock(shared_lock&& other) noexcept;
  auto operator=(shared_lock&& other) noexcept -> shared_lock&;

  [[nodiscard]] auto is_held() const noexcept -> bool { return fd_ >= 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return is_held(); }

  void release() noexcept;

private:
  explicit shared_lock(int fd) noexcept;

  int fd_ = -1;
};

// ============================================================================
// lock_guard - Scoped lock helper
// ============================================================================

/// RAII wrapper that acquires lock on construction, releases on destruction.
/// Throws on failure (for use in contexts where failure is exceptional).
///
/// Usage:
///   void write_to_db() {
///       lock_guard guard(db_lock_path);  // Throws if can't acquire
///       // ... critical section ...
///   }  // Released
///
class lock_guard {
public:
  explicit lock_guard(const std::filesystem::path& path);
  ~lock_guard() = default;

  lock_guard(const lock_guard&) = delete;
  auto operator=(const lock_guard&) -> lock_guard& = delete;
  lock_guard(lock_guard&&) = default;
  auto operator=(lock_guard&&) -> lock_guard& = default;

private:
  exclusive_lock lock_;
};

// ============================================================================
// Implementation
// ============================================================================

inline exclusive_lock::exclusive_lock(int fd, std::filesystem::path path, pid_t pid) noexcept
    : fd_(fd), path_(std::move(path)), owning_pid_(pid) {}

inline exclusive_lock::exclusive_lock(exclusive_lock&& other) noexcept
    : fd_(other.fd_), path_(std::move(other.path_)), owning_pid_(other.owning_pid_) {
  other.fd_ = -1;
  other.owning_pid_ = 0;
}

inline auto exclusive_lock::operator=(exclusive_lock&& other) noexcept -> exclusive_lock& {
  if (this != &other) {
    release();
    fd_ = other.fd_;
    path_ = std::move(other.path_);
    owning_pid_ = other.owning_pid_;
    other.fd_ = -1;
    other.owning_pid_ = 0;
  }
  return *this;
}

inline exclusive_lock::~exclusive_lock() {
  release();
}

inline void exclusive_lock::release() noexcept {
  if (fd_ >= 0 && owning_pid_ == ::getpid()) {
    // Only release if we're the original acquirer (not a forked child)
    ::close(fd_);
  }
  fd_ = -1;
  owning_pid_ = 0;
}

inline void exclusive_lock::write_pid() noexcept {
  if (fd_ < 0)
    return;

  ::ftruncate(fd_, 0);
  ::lseek(fd_, 0, SEEK_SET);

  auto s = std::to_string(::getpid());
  [[maybe_unused]] auto written = ::write(fd_, s.c_str(), s.size());
  ::fsync(fd_);
}

inline auto exclusive_lock::read_pid(int fd) noexcept -> pid_t {
  char buf[32] = {0};
  ::lseek(fd, 0, SEEK_SET);
  [[maybe_unused]] auto n = ::read(fd, buf, sizeof(buf) - 1);
  return static_cast<pid_t>(std::atoi(buf));
}

inline auto exclusive_lock::is_process_alive(pid_t pid) noexcept -> bool {
  if (pid <= 0)
    return false;
  return ::kill(pid, 0) == 0 || errno != ESRCH;
}

inline auto exclusive_lock::acquire(const std::filesystem::path& path)
    -> lock_result<exclusive_lock> {
  // Open or create the lock file
  // O_CLOEXEC: don't leak fd across exec
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return std::unexpected(lock_error::open_failed);
  }

  // Retry loop for robustness
  constexpr int max_retries = 100;
  constexpr auto retry_delay = std::chrono::milliseconds(10);

  for (int attempt = 0; attempt < max_retries; ++attempt) {
    // Try non-blocking acquire first
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      // Got the lock
      pid_t pid = ::getpid();
      exclusive_lock lock(fd, path, pid);
      lock.write_pid();
      return lock;
    }

    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      ::close(fd);
      return std::unexpected(lock_error::lock_failed);
    }

    // Lock held by someone else - check if they're alive
    pid_t holder = read_pid(fd);
    if (holder > 0 && !is_process_alive(holder)) {
      // Holder is dead but lock not released (NFS? bug?)
      // The kernel SHOULD have released flock on process death.
      // If we're here, something is weird. Brief retry.
      std::this_thread::sleep_for(retry_delay);
      continue;
    }

    // Holder is alive (or unknown), do a blocking wait
    if (::flock(fd, LOCK_EX) == 0) {
      // Got the lock after waiting
      pid_t pid = ::getpid();
      exclusive_lock lock(fd, path, pid);
      lock.write_pid();
      return lock;
    }

    if (errno == EINTR) {
      continue; // Interrupted, retry
    }

    ::close(fd);
    return std::unexpected(lock_error::lock_failed);
  }

  ::close(fd);
  return std::unexpected(lock_error::lock_failed);
}

inline auto exclusive_lock::try_acquire(const std::filesystem::path& path)
    -> lock_result<exclusive_lock> {
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return std::unexpected(lock_error::open_failed);
  }

  if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
    pid_t pid = ::getpid();
    exclusive_lock lock(fd, path, pid);
    lock.write_pid();
    return lock;
  }

  ::close(fd);
  return std::unexpected(lock_error::lock_failed);
}

// ============================================================================
// shared_lock implementation
// ============================================================================

inline shared_lock::shared_lock(int fd) noexcept : fd_(fd) {}

inline shared_lock::shared_lock(shared_lock&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

inline auto shared_lock::operator=(shared_lock&& other) noexcept -> shared_lock& {
  if (this != &other) {
    release();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

inline shared_lock::~shared_lock() {
  release();
}

inline void shared_lock::release() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = -1;
}

inline auto shared_lock::acquire(const std::filesystem::path& path) -> lock_result<shared_lock> {
  int fd = ::open(path.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return std::unexpected(lock_error::open_failed);
  }

  if (::flock(fd, LOCK_SH) == 0) {
    return shared_lock(fd);
  }

  ::close(fd);
  return std::unexpected(lock_error::lock_failed);
}

inline auto shared_lock::try_acquire(const std::filesystem::path& path)
    -> lock_result<shared_lock> {
  int fd = ::open(path.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return std::unexpected(lock_error::open_failed);
  }

  if (::flock(fd, LOCK_SH | LOCK_NB) == 0) {
    return shared_lock(fd);
  }

  ::close(fd);
  return std::unexpected(lock_error::lock_failed);
}

// ============================================================================
// lock_guard implementation
// ============================================================================

inline lock_guard::lock_guard(const std::filesystem::path& path)
    : lock_(exclusive_lock::acquire(path).value()) {}

} // namespace straylight::nix::sync
