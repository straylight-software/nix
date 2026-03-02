// straylight::nix::fs::file_lock
//
// RAII file locking primitives using POSIX flock().
//
// Provides exclusive and shared file locks with automatic release.
// Designed for Nix store path locking scenarios.
//
// Usage:
//   auto lock = file_lock::exclusive("/nix/store/.links/.lock");
//   // lock held until destruction
//
//   if (auto lock = file_lock::try_exclusive(path)) {
//     // acquired lock
//   }

#pragma once

#include <cerrno>
#include <chrono>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace straylight::nix::fs {

// ─────────────────────────────────────────────────────────────────────────────
// Lock types
// ─────────────────────────────────────────────────────────────────────────────

enum class LockMode {
  Shared,   // LOCK_SH - multiple readers
  Exclusive // LOCK_EX - single writer
};

// ─────────────────────────────────────────────────────────────────────────────
// Error handling
// ─────────────────────────────────────────────────────────────────────────────

enum class LockError {
  FileNotFound,
  PermissionDenied,
  WouldBlock,  // Non-blocking acquire failed
  Interrupted, // EINTR
  InvalidDescriptor,
  DeadlockWouldOccur,
  SystemError
};

[[nodiscard]] inline std::string_view to_string(LockError e) noexcept {
  switch (e) {
    case LockError::FileNotFound:
      return "file not found";
    case LockError::PermissionDenied:
      return "permission denied";
    case LockError::WouldBlock:
      return "lock already held";
    case LockError::Interrupted:
      return "interrupted";
    case LockError::InvalidDescriptor:
      return "invalid file descriptor";
    case LockError::DeadlockWouldOccur:
      return "deadlock would occur";
    case LockError::SystemError:
      return "system error";
  }
  return "unknown error";
}

[[nodiscard]] inline LockError lock_error_from_errno(int err) noexcept {
  switch (err) {
    case ENOENT:
      return LockError::FileNotFound;
    case EACCES:
    case EPERM:
      return LockError::PermissionDenied;
    case EWOULDBLOCK:
      return LockError::WouldBlock;
    case EINTR:
      return LockError::Interrupted;
    case EBADF:
      return LockError::InvalidDescriptor;
    case EDEADLK:
      return LockError::DeadlockWouldOccur;
    default:
      return LockError::SystemError;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// FileLock - RAII lock on an open file descriptor
// ─────────────────────────────────────────────────────────────────────────────

/// RAII wrapper for a file lock. Does NOT own the file descriptor.
/// Use when you already have an open fd and want to lock it.
class FdLock {
public:
  FdLock() noexcept = default;

  /// Acquire lock on existing fd (blocking)
  [[nodiscard]] static std::expected<FdLock, LockError> acquire(int fd, LockMode mode) noexcept {
    int op = (mode == LockMode::Exclusive) ? LOCK_EX : LOCK_SH;
    while (::flock(fd, op) != 0) {
      if (errno != EINTR) {
        return std::unexpected(lock_error_from_errno(errno));
      }
      // Retry on EINTR
    }
    return FdLock(fd, true);
  }

  /// Try to acquire lock (non-blocking)
  [[nodiscard]] static std::expected<FdLock, LockError> try_acquire(int fd,
                                                                    LockMode mode) noexcept {
    int op = ((mode == LockMode::Exclusive) ? LOCK_EX : LOCK_SH) | LOCK_NB;
    if (::flock(fd, op) != 0) {
      return std::unexpected(lock_error_from_errno(errno));
    }
    return FdLock(fd, true);
  }

  ~FdLock() { release(); }

  // Move-only
  FdLock(FdLock&& other) noexcept : fd_(other.fd_), held_(other.held_) { other.held_ = false; }

  FdLock& operator=(FdLock&& other) noexcept {
    if (this != &other) {
      release();
      fd_ = other.fd_;
      held_ = other.held_;
      other.held_ = false;
    }
    return *this;
  }

  FdLock(const FdLock&) = delete;
  FdLock& operator=(const FdLock&) = delete;

  /// Release the lock early
  void release() noexcept {
    if (held_) {
      ::flock(fd_, LOCK_UN);
      held_ = false;
    }
  }

  /// Check if lock is held
  [[nodiscard]] bool held() const noexcept { return held_; }
  [[nodiscard]] explicit operator bool() const noexcept { return held_; }

  /// Get the underlying fd
  [[nodiscard]] int fd() const noexcept { return fd_; }

private:
  explicit FdLock(int fd, bool held) noexcept : fd_(fd), held_(held) {}

  int fd_ = -1;
  bool held_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// FileLock - RAII lock that owns the file descriptor
// ─────────────────────────────────────────────────────────────────────────────

/// RAII wrapper that opens a lock file, acquires a lock, and closes on destruction.
/// Use for path-based locking (e.g., /nix/store/.links/.lock).
class FileLock {
public:
  FileLock() noexcept = default;

  /// Open and lock a file (blocking). Creates file if it doesn't exist.
  [[nodiscard]] static std::expected<FileLock, LockError> acquire(const std::filesystem::path& path,
                                                                  LockMode mode) noexcept {
    int flags = O_RDWR | O_CREAT | O_CLOEXEC;
    int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) {
      return std::unexpected(lock_error_from_errno(errno));
    }

    int op = (mode == LockMode::Exclusive) ? LOCK_EX : LOCK_SH;
    while (::flock(fd, op) != 0) {
      if (errno != EINTR) {
        auto err = lock_error_from_errno(errno);
        ::close(fd);
        return std::unexpected(err);
      }
    }
    return FileLock(fd, path, mode);
  }

  /// Try to open and lock a file (non-blocking)
  [[nodiscard]] static std::expected<FileLock, LockError>
  try_acquire(const std::filesystem::path& path, LockMode mode) noexcept {
    int flags = O_RDWR | O_CREAT | O_CLOEXEC;
    int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) {
      return std::unexpected(lock_error_from_errno(errno));
    }

    int op = ((mode == LockMode::Exclusive) ? LOCK_EX : LOCK_SH) | LOCK_NB;
    if (::flock(fd, op) != 0) {
      auto err = lock_error_from_errno(errno);
      ::close(fd);
      return std::unexpected(err);
    }
    return FileLock(fd, path, mode);
  }

  /// Convenience: exclusive lock (blocking)
  [[nodiscard]] static std::expected<FileLock, LockError>
  exclusive(const std::filesystem::path& path) noexcept {
    return acquire(path, LockMode::Exclusive);
  }

  /// Convenience: shared lock (blocking)
  [[nodiscard]] static std::expected<FileLock, LockError>
  shared(const std::filesystem::path& path) noexcept {
    return acquire(path, LockMode::Shared);
  }

  /// Convenience: try exclusive lock (non-blocking)
  [[nodiscard]] static std::expected<FileLock, LockError>
  try_exclusive(const std::filesystem::path& path) noexcept {
    return try_acquire(path, LockMode::Exclusive);
  }

  /// Convenience: try shared lock (non-blocking)
  [[nodiscard]] static std::expected<FileLock, LockError>
  try_shared(const std::filesystem::path& path) noexcept {
    return try_acquire(path, LockMode::Shared);
  }

  ~FileLock() { release(); }

  // Move-only
  FileLock(FileLock&& other) noexcept
      : fd_(other.fd_), path_(std::move(other.path_)), mode_(other.mode_) {
    other.fd_ = -1;
  }

  FileLock& operator=(FileLock&& other) noexcept {
    if (this != &other) {
      release();
      fd_ = other.fd_;
      path_ = std::move(other.path_);
      mode_ = other.mode_;
      other.fd_ = -1;
    }
    return *this;
  }

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  /// Release the lock and close the file
  void release() noexcept {
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
      fd_ = -1;
    }
  }

  /// Upgrade shared lock to exclusive (blocking)
  [[nodiscard]] std::expected<void, LockError> upgrade() noexcept {
    if (fd_ < 0) {
      return std::unexpected(LockError::InvalidDescriptor);
    }
    if (mode_ == LockMode::Exclusive) {
      return {}; // Already exclusive
    }

    while (::flock(fd_, LOCK_EX) != 0) {
      if (errno != EINTR) {
        return std::unexpected(lock_error_from_errno(errno));
      }
    }
    mode_ = LockMode::Exclusive;
    return {};
  }

  /// Downgrade exclusive lock to shared
  [[nodiscard]] std::expected<void, LockError> downgrade() noexcept {
    if (fd_ < 0) {
      return std::unexpected(LockError::InvalidDescriptor);
    }
    if (mode_ == LockMode::Shared) {
      return {}; // Already shared
    }

    while (::flock(fd_, LOCK_SH) != 0) {
      if (errno != EINTR) {
        return std::unexpected(lock_error_from_errno(errno));
      }
    }
    mode_ = LockMode::Shared;
    return {};
  }

  /// Check if lock is held
  [[nodiscard]] bool held() const noexcept { return fd_ >= 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return held(); }

  /// Get the lock mode
  [[nodiscard]] LockMode mode() const noexcept { return mode_; }

  /// Get the locked path
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  /// Get the underlying fd
  [[nodiscard]] int fd() const noexcept { return fd_; }

private:
  FileLock(int fd, std::filesystem::path path, LockMode mode) noexcept
      : fd_(fd), path_(std::move(path)), mode_(mode) {}

  int fd_ = -1;
  std::filesystem::path path_;
  LockMode mode_ = LockMode::Exclusive;
};

// ─────────────────────────────────────────────────────────────────────────────
// Multi-path locking (for atomic operations on multiple store paths)
// ─────────────────────────────────────────────────────────────────────────────

/// Lock multiple paths atomically (in sorted order to prevent deadlocks)
template <typename PathContainer>
[[nodiscard]] std::expected<std::vector<FileLock>, LockError> lock_paths(const PathContainer& paths,
                                                                         LockMode mode) {
  // Sort paths to prevent deadlocks (consistent ordering)
  std::vector<std::filesystem::path> sorted_paths(paths.begin(), paths.end());
  std::sort(sorted_paths.begin(), sorted_paths.end());

  std::vector<FileLock> locks;
  locks.reserve(sorted_paths.size());

  for (const auto& path : sorted_paths) {
    auto result = FileLock::acquire(path, mode);
    if (!result) {
      // Release all acquired locks on failure
      locks.clear();
      return std::unexpected(result.error());
    }
    locks.push_back(std::move(*result));
  }

  return locks;
}

} // namespace straylight::nix::fs
