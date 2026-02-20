// straylight::nix::fs::mmap
//
// Memory-mapped file primitives using mio (header-only, zero deps).
//
// Provides read-only and read-write memory-mapped files with automatic
// unmapping on destruction. Uses mio internally for cross-platform support.
//
// Usage:
//   auto view = mmap::map_read("/nix/store/...-foo/bar.so");
//   std::span<const std::byte> data = view->data();
//
//   auto rw = mmap::map_write("output.bin", 4096);
//   std::memcpy(rw->data().data(), src, 4096);

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <system_error>

// mio - header-only memory mapping library (vendored)
#include "mio/mio.hpp"

namespace straylight::nix::fs {

// ─────────────────────────────────────────────────────────────────────────────
// Error handling
// ─────────────────────────────────────────────────────────────────────────────

enum class MmapError { FileNotFound, PermissionDenied, InvalidSize, MappingFailed, SystemError };

[[nodiscard]] inline std::string_view to_string(MmapError e) noexcept {
  switch (e) {
    case MmapError::FileNotFound:
      return "file not found";
    case MmapError::PermissionDenied:
      return "permission denied";
    case MmapError::InvalidSize:
      return "invalid size";
    case MmapError::MappingFailed:
      return "mapping failed";
    case MmapError::SystemError:
      return "system error";
  }
  return "unknown error";
}

[[nodiscard]] inline MmapError mmap_error_from_std(const std::error_code& ec) noexcept {
  if (ec == std::errc::no_such_file_or_directory) {
    return MmapError::FileNotFound;
  } else if (ec == std::errc::permission_denied) {
    return MmapError::PermissionDenied;
  } else if (ec == std::errc::invalid_argument) {
    return MmapError::InvalidSize;
  }
  return MmapError::MappingFailed;
}

// ─────────────────────────────────────────────────────────────────────────────
// MappedFile - read-only memory-mapped file
// ─────────────────────────────────────────────────────────────────────────────

/// Read-only memory-mapped file
class MappedFileRead {
public:
  MappedFileRead() = default;

  /// Map entire file for reading
  [[nodiscard]] static std::expected<MappedFileRead, MmapError>
  open(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    mio::mmap_source mapping = mio::make_mmap_source(path.string(), ec);
    if (ec) {
      return std::unexpected(mmap_error_from_std(ec));
    }
    return MappedFileRead(std::move(mapping), path);
  }

  /// Map a portion of a file for reading
  [[nodiscard]] static std::expected<MappedFileRead, MmapError>
  open(const std::filesystem::path& path, std::size_t offset, std::size_t length) noexcept {
    std::error_code ec;
    mio::mmap_source mapping = mio::make_mmap_source(path.string(), offset, length, ec);
    if (ec) {
      return std::unexpected(mmap_error_from_std(ec));
    }
    return MappedFileRead(std::move(mapping), path);
  }

  /// Map from an already-open file descriptor
  [[nodiscard]] static std::expected<MappedFileRead, MmapError>
  from_fd(int fd, std::size_t offset = 0, std::size_t length = mio::map_entire_file) noexcept {
    std::error_code ec;
    mio::mmap_source mapping;
    mapping.map(fd, offset, length, ec);
    if (ec) {
      return std::unexpected(mmap_error_from_std(ec));
    }
    return MappedFileRead(std::move(mapping), {});
  }

  // Move-only
  MappedFileRead(MappedFileRead&&) = default;
  MappedFileRead& operator=(MappedFileRead&&) = default;
  MappedFileRead(const MappedFileRead&) = delete;
  MappedFileRead& operator=(const MappedFileRead&) = delete;

  /// Get the mapped data as a span of bytes
  [[nodiscard]] std::span<const std::byte> data() const noexcept {
    return {reinterpret_cast<const std::byte*>(mapping_.data()), mapping_.size()};
  }

  /// Get the mapped data as a span of chars (for text files)
  [[nodiscard]] std::span<const char> chars() const noexcept {
    return {mapping_.data(), mapping_.size()};
  }

  /// Get as string_view (for text files)
  [[nodiscard]] std::string_view string_view() const noexcept {
    return {mapping_.data(), mapping_.size()};
  }

  /// Get the size of the mapping
  [[nodiscard]] std::size_t size() const noexcept { return mapping_.size(); }

  /// Check if mapping is valid
  [[nodiscard]] bool is_mapped() const noexcept { return mapping_.is_mapped(); }
  [[nodiscard]] explicit operator bool() const noexcept { return is_mapped(); }

  /// Get the path (if known)
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  /// Unmap the file
  void unmap() noexcept { mapping_.unmap(); }

private:
  MappedFileRead(mio::mmap_source mapping, std::filesystem::path path) noexcept
      : mapping_(std::move(mapping)), path_(std::move(path)) {}

  mio::mmap_source mapping_;
  std::filesystem::path path_;
};

// ─────────────────────────────────────────────────────────────────────────────
// MappedFileWrite - read-write memory-mapped file
// ─────────────────────────────────────────────────────────────────────────────

/// Read-write memory-mapped file
class MappedFileWrite {
public:
  MappedFileWrite() = default;

  /// Map entire file for reading and writing
  [[nodiscard]] static std::expected<MappedFileWrite, MmapError>
  open(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    mio::mmap_sink mapping = mio::make_mmap_sink(path.string(), ec);
    if (ec) {
      return std::unexpected(mmap_error_from_std(ec));
    }
    return MappedFileWrite(std::move(mapping), path);
  }

  /// Map a portion of a file for reading and writing
  [[nodiscard]] static std::expected<MappedFileWrite, MmapError>
  open(const std::filesystem::path& path, std::size_t offset, std::size_t length) noexcept {
    std::error_code ec;
    mio::mmap_sink mapping = mio::make_mmap_sink(path.string(), offset, length, ec);
    if (ec) {
      return std::unexpected(mmap_error_from_std(ec));
    }
    return MappedFileWrite(std::move(mapping), path);
  }

  /// Map from an already-open file descriptor
  [[nodiscard]] static std::expected<MappedFileWrite, MmapError>
  from_fd(int fd, std::size_t offset = 0, std::size_t length = mio::map_entire_file) noexcept {
    std::error_code ec;
    mio::mmap_sink mapping;
    mapping.map(fd, offset, length, ec);
    if (ec) {
      return std::unexpected(mmap_error_from_std(ec));
    }
    return MappedFileWrite(std::move(mapping), {});
  }

  // Move-only
  MappedFileWrite(MappedFileWrite&&) = default;
  MappedFileWrite& operator=(MappedFileWrite&&) = default;
  MappedFileWrite(const MappedFileWrite&) = delete;
  MappedFileWrite& operator=(const MappedFileWrite&) = delete;

  /// Get the mapped data as a mutable span of bytes
  [[nodiscard]] std::span<std::byte> data() noexcept {
    return {reinterpret_cast<std::byte*>(mapping_.data()), mapping_.size()};
  }

  /// Get the mapped data as a const span of bytes
  [[nodiscard]] std::span<const std::byte> data() const noexcept {
    return {reinterpret_cast<const std::byte*>(mapping_.data()), mapping_.size()};
  }

  /// Get the mapped data as a mutable span of chars
  [[nodiscard]] std::span<char> chars() noexcept { return {mapping_.data(), mapping_.size()}; }

  /// Get the size of the mapping
  [[nodiscard]] std::size_t size() const noexcept { return mapping_.size(); }

  /// Check if mapping is valid
  [[nodiscard]] bool is_mapped() const noexcept { return mapping_.is_mapped(); }
  [[nodiscard]] explicit operator bool() const noexcept { return is_mapped(); }

  /// Get the path (if known)
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  /// Sync changes to disk
  void sync() noexcept {
    std::error_code ec;
    mapping_.sync(ec);
  }

  /// Unmap the file
  void unmap() noexcept { mapping_.unmap(); }

private:
  MappedFileWrite(mio::mmap_sink mapping, std::filesystem::path path) noexcept
      : mapping_(std::move(mapping)), path_(std::move(path)) {}

  mio::mmap_sink mapping_;
  std::filesystem::path path_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Convenience functions
// ─────────────────────────────────────────────────────────────────────────────

/// Map a file for reading
[[nodiscard]] inline std::expected<MappedFileRead, MmapError>
map_read(const std::filesystem::path& path) noexcept {
  return MappedFileRead::open(path);
}

/// Map a file for reading and writing
[[nodiscard]] inline std::expected<MappedFileWrite, MmapError>
map_write(const std::filesystem::path& path) noexcept {
  return MappedFileWrite::open(path);
}

/// Get the system page size
[[nodiscard]] inline std::size_t page_size() noexcept {
  return mio::page_size();
}

} // namespace straylight::nix::fs
