// straylight::nix::fs::temp
//
// RAII temporary file and directory primitives.
//
// Automatically creates unique temp files/dirs and removes them on destruction.
// Uses POSIX mkstemp/mkdtemp for secure creation.
//
// Usage:
//   auto tmp = TempFile::create();
//   write(tmp->fd(), data, size);
//   // file removed on destruction
//
//   auto dir = TempDir::create();
//   // use dir->path() / "subfile"
//   // directory recursively removed on destruction

#pragma once

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::nix::fs {

// ─────────────────────────────────────────────────────────────────────────────
// Error handling
// ─────────────────────────────────────────────────────────────────────────────

enum class TempError {
  NoTempDir,      // TMPDIR not set and no fallback
  CreationFailed, // mkstemp/mkdtemp failed
  PermissionDenied,
  NoSpace,
  SystemError
};

[[nodiscard]] inline std::string_view to_string(TempError e) noexcept {
  switch (e) {
    case TempError::NoTempDir:
      return "no temp directory available";
    case TempError::CreationFailed:
      return "temp file/dir creation failed";
    case TempError::PermissionDenied:
      return "permission denied";
    case TempError::NoSpace:
      return "no space left on device";
    case TempError::SystemError:
      return "system error";
  }
  return "unknown error";
}

[[nodiscard]] inline TempError temp_error_from_errno(int err) noexcept {
  switch (err) {
    case EACCES:
    case EPERM:
      return TempError::PermissionDenied;
    case ENOSPC:
      return TempError::NoSpace;
    case ENOENT:
    case ENOTDIR:
      return TempError::NoTempDir;
    default:
      return TempError::CreationFailed;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Get the system temp directory (TMPDIR, or /tmp as fallback)
/// Validates that the directory exists, falling back to /tmp if it doesn't.
/// This handles stale TMPDIR from exited nix-shell sessions.
[[nodiscard]] inline std::filesystem::path temp_directory() noexcept {
  auto try_dir = [](const char* env_var) -> std::filesystem::path {
    if (const char* val = std::getenv(env_var)) {
      std::error_code ec;
      if (std::filesystem::exists(val, ec) && !ec) {
        return val;
      }
    }
    return {};
  };

  if (auto p = try_dir("TMPDIR"); !p.empty()) {
    return p;
  }
  if (auto p = try_dir("TMP"); !p.empty()) {
    return p;
  }
  if (auto p = try_dir("TEMP"); !p.empty()) {
    return p;
  }

  return "/tmp";
}

namespace detail {
// Atomic counter for unique temp names (fallback for patterns)
inline std::atomic<std::uint64_t> temp_counter{0};
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// TempFile - RAII temporary file
// ─────────────────────────────────────────────────────────────────────────────

/// RAII temporary file that is automatically deleted on destruction.
class TempFile {
public:
  TempFile() = default;

  /// Create a temp file with a prefix in the system temp directory
  [[nodiscard]] static std::expected<TempFile, TempError>
  create(std::string_view prefix = "straylight") noexcept {
    return create_in(temp_directory(), prefix);
  }

  /// Create a temp file with a prefix in a specific directory
  [[nodiscard]] static std::expected<TempFile, TempError>
  create_in(const std::filesystem::path& dir, std::string_view prefix = "straylight") noexcept {
    // Build template: dir/prefix.XXXXXX
    std::string tmpl = (dir / (std::string(prefix) + ".XXXXXX")).string();

    // mkstemp modifies the template in place
    int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
      return std::unexpected(temp_error_from_errno(errno));
    }

    return TempFile(fd, std::filesystem::path(tmpl), true);
  }

  /// Create an anonymous temp file (Linux 3.11+ with O_TMPFILE)
  /// The file has no name and is automatically deleted when closed.
  [[nodiscard]] static std::expected<TempFile, TempError>
  create_anonymous(const std::filesystem::path& dir = {}) noexcept {
#ifdef O_TMPFILE
    std::filesystem::path target = dir.empty() ? temp_directory() : dir;
    int fd = ::open(target.c_str(), O_RDWR | O_TMPFILE | O_CLOEXEC, 0600);
    if (fd >= 0) {
      return TempFile(fd, {}, false); // No path to delete
    }
    // Fall through to mkstemp if O_TMPFILE not supported
#endif
    // Fallback to regular temp file
    std::filesystem::path target_dir = dir.empty() ? temp_directory() : dir;
    return create_in(target_dir, "anon");
  }

  ~TempFile() { close_and_remove(); }

  // Move-only
  TempFile(TempFile&& other) noexcept
      : fd_(other.fd_), path_(std::move(other.path_)), delete_on_close_(other.delete_on_close_) {
    other.fd_ = -1;
    other.delete_on_close_ = false;
  }

  TempFile& operator=(TempFile&& other) noexcept {
    if (this != &other) {
      close_and_remove();
      fd_ = other.fd_;
      path_ = std::move(other.path_);
      delete_on_close_ = other.delete_on_close_;
      other.fd_ = -1;
      other.delete_on_close_ = false;
    }
    return *this;
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  /// Get the file descriptor
  [[nodiscard]] int fd() const noexcept { return fd_; }

  /// Get the file path (empty for anonymous files)
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  /// Check if valid
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return is_open(); }

  /// Release ownership - file will NOT be deleted on destruction
  [[nodiscard]] std::pair<int, std::filesystem::path> release() noexcept {
    int fd = fd_;
    std::filesystem::path p = std::move(path_);
    fd_ = -1;
    delete_on_close_ = false;
    return {fd, p};
  }

  /// Keep the file (don't delete on destruction) but close the fd
  void keep() noexcept {
    delete_on_close_ = false;
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  /// Close and delete now
  void close_and_remove() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (delete_on_close_ && !path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
      // Ignore errors - best effort cleanup
    }
    delete_on_close_ = false;
    path_.clear();
  }

private:
  TempFile(int fd, std::filesystem::path path, bool delete_on_close) noexcept
      : fd_(fd), path_(std::move(path)), delete_on_close_(delete_on_close) {}

  int fd_ = -1;
  std::filesystem::path path_;
  bool delete_on_close_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// TempDir - RAII temporary directory
// ─────────────────────────────────────────────────────────────────────────────

/// RAII temporary directory that is recursively deleted on destruction.
class TempDir {
public:
  TempDir() = default;

  /// Create a temp directory with a prefix in the system temp directory
  [[nodiscard]] static std::expected<TempDir, TempError>
  create(std::string_view prefix = "straylight") noexcept {
    return create_in(temp_directory(), prefix);
  }

  /// Create a temp directory with a prefix in a specific directory
  [[nodiscard]] static std::expected<TempDir, TempError>
  create_in(const std::filesystem::path& dir, std::string_view prefix = "straylight") noexcept {
    // Build template: dir/prefix.XXXXXX
    std::string tmpl = (dir / (std::string(prefix) + ".XXXXXX")).string();

    // mkdtemp modifies the template in place
    char* result = ::mkdtemp(tmpl.data());
    if (result == nullptr) {
      return std::unexpected(temp_error_from_errno(errno));
    }

    return TempDir(std::filesystem::path(tmpl), true);
  }

  ~TempDir() { remove_recursive(); }

  // Move-only
  TempDir(TempDir&& other) noexcept
      : path_(std::move(other.path_)), delete_on_close_(other.delete_on_close_) {
    other.delete_on_close_ = false;
  }

  TempDir& operator=(TempDir&& other) noexcept {
    if (this != &other) {
      remove_recursive();
      path_ = std::move(other.path_);
      delete_on_close_ = other.delete_on_close_;
      other.delete_on_close_ = false;
    }
    return *this;
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  /// Get the directory path
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  /// Convenience: path / subpath
  [[nodiscard]] std::filesystem::path operator/(const std::filesystem::path& subpath) const {
    return path_ / subpath;
  }

  /// Check if valid
  [[nodiscard]] bool is_valid() const noexcept { return !path_.empty(); }
  [[nodiscard]] explicit operator bool() const noexcept { return is_valid(); }

  /// Release ownership - directory will NOT be deleted on destruction
  [[nodiscard]] std::filesystem::path release() noexcept {
    std::filesystem::path p = std::move(path_);
    delete_on_close_ = false;
    return p;
  }

  /// Keep the directory (don't delete on destruction)
  void keep() noexcept { delete_on_close_ = false; }

  /// Delete now
  void remove_recursive() noexcept {
    if (delete_on_close_ && !path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
      // Ignore errors - best effort cleanup
    }
    delete_on_close_ = false;
    path_.clear();
  }

private:
  explicit TempDir(std::filesystem::path path, bool delete_on_close) noexcept
      : path_(std::move(path)), delete_on_close_(delete_on_close) {}

  std::filesystem::path path_;
  bool delete_on_close_ = false;
};

} // namespace straylight::nix::fs
