// straylight::nix::primitives::ca_store implementation
//
// Content-addressed blob storage with:
// - Atomic writes via rename()
// - No coordination (idempotent by design)
// - Optional io_uring acceleration for bulk ops

#include "ca_store.h"

#include <algorithm>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::nix::store {

// ============================================================================
// Initialization
// ============================================================================

auto ca_store::init() -> ca_result<void> {
  namespace fs = std::filesystem;

  std::error_code ec;

  // Create root directory
  fs::create_directories(root_, ec);
  if (ec) {
    return std::unexpected(ca_error::io_error);
  }

  // Create shard directories (00-ff)
  char shard[3] = {0};
  for (int i = 0; i < 256; ++i) {
    shard[0] = "0123456789abcdef"[(i >> 4) & 0xf];
    shard[1] = "0123456789abcdef"[i & 0xf];
    fs::create_directories(root_ / shard, ec);
    if (ec) {
      return std::unexpected(ca_error::io_error);
    }
  }

  // Clean up any incomplete writes
  auto cleaned = cleanup_temps();
  if (!cleaned) {
    return std::unexpected(cleaned.error());
  }

  return {};
}

// ============================================================================
// Core operations
// ============================================================================

auto ca_store::put(std::span<const std::byte> data) -> ca_result<std::string> {
  // Hash the content using BLAKE3
  auto digest = hash::compute(hash_algorithm,
                              std::span<const std::uint8_t>(
                                  reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
  std::string hash_hex = digest.to_hex();

  // Write the blob
  auto result = write_blob(hash_hex, data);
  if (!result) {
    return std::unexpected(result.error());
  }

  return hash_hex;
}

auto ca_store::put(std::string_view hash, std::span<const std::byte> data) -> ca_result<void> {
  if (!is_valid_hash(hash)) {
    return std::unexpected(ca_error::invalid_hash);
  }

  // Verify hash matches content
  auto digest = hash::compute(hash_algorithm,
                              std::span<const std::uint8_t>(
                                  reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
  std::string computed = digest.to_hex();

  if (computed != hash) {
    return std::unexpected(ca_error::hash_mismatch);
  }

  return write_blob(hash, data);
}

auto ca_store::get(std::string_view hash) -> ca_result<std::vector<std::byte>> {
  if (!is_valid_hash(hash)) {
    return std::unexpected(ca_error::invalid_hash);
  }
  return read_blob(hash);
}

auto ca_store::remove(std::string_view hash) -> ca_result<bool> {
  if (!is_valid_hash(hash)) {
    return std::unexpected(ca_error::invalid_hash);
  }

  auto path = blob_path(hash);
  std::error_code ec;

  if (!std::filesystem::exists(path, ec)) {
    return false;
  }

  if (!std::filesystem::remove(path, ec) || ec) {
    return std::unexpected(ca_error::io_error);
  }

  return true;
}

// ============================================================================
// Internal write/read
// ============================================================================

auto ca_store::write_blob(std::string_view hash, std::span<const std::byte> data)
    -> ca_result<void> {
  auto final_path = blob_path(hash);
  auto tmp = temp_path(hash);

  // If blob already exists, we're done (idempotent)
  std::error_code ec;
  if (std::filesystem::exists(final_path, ec)) {
    return {};
  }

  // Open temp file
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return std::unexpected(ca_error::io_error);
  }

  // Write data
  const auto* ptr = reinterpret_cast<const char*>(data.data());
  std::size_t remaining = data.size();

  while (remaining > 0) {
    ssize_t written = ::write(fd, ptr, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      std::filesystem::remove(tmp, ec);
      return std::unexpected(ca_error::io_error);
    }
    ptr += written;
    remaining -= static_cast<std::size_t>(written);
  }

  // fsync the file
  if (::fsync(fd) < 0) {
    ::close(fd);
    std::filesystem::remove(tmp, ec);
    return std::unexpected(ca_error::io_error);
  }

  ::close(fd);

  // Atomic rename
  if (::rename(tmp.c_str(), final_path.c_str()) < 0) {
    // ENOENT means another process already renamed it - that's fine
    if (errno != ENOENT && !std::filesystem::exists(final_path, ec)) {
      std::filesystem::remove(tmp, ec);
      return std::unexpected(ca_error::io_error);
    }
  }

  // fsync parent directory for crash safety
  auto parent = final_path.parent_path();
  int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd >= 0) {
    ::fsync(dir_fd);
    ::close(dir_fd);
  }

  return {};
}

auto ca_store::read_blob(std::string_view hash) -> ca_result<std::vector<std::byte>> {
  auto path = blob_path(hash);

  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      return std::unexpected(ca_error::not_found);
    }
    return std::unexpected(ca_error::io_error);
  }

  // Get file size
  struct stat st{};
  if (::fstat(fd, &st) < 0) {
    ::close(fd);
    return std::unexpected(ca_error::io_error);
  }

  std::vector<std::byte> data(static_cast<std::size_t>(st.st_size));
  auto* ptr = reinterpret_cast<char*>(data.data());
  std::size_t remaining = data.size();

  while (remaining > 0) {
    ssize_t n = ::read(fd, ptr, remaining);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return std::unexpected(ca_error::io_error);
    }
    if (n == 0) {
      break; // EOF
    }
    ptr += n;
    remaining -= static_cast<std::size_t>(n);
  }

  ::close(fd);
  return data;
}

// ============================================================================
// Bulk operations
// ============================================================================

auto ca_store::bulk_has(std::span<const std::string> hashes) -> std::vector<bool> {
  std::vector<bool> results;
  results.reserve(hashes.size());

  // TODO: io_uring statx batching when ring_ is available
  for (const auto& hash : hashes) {
    results.push_back(has(hash));
  }

  return results;
}

auto ca_store::bulk_get(std::span<const std::string> hashes)
    -> std::vector<ca_result<std::vector<std::byte>>> {
  std::vector<ca_result<std::vector<std::byte>>> results;
  results.reserve(hashes.size());

  // TODO: io_uring read batching when ring_ is available
  for (const auto& hash : hashes) {
    results.push_back(get(hash));
  }

  return results;
}

// ============================================================================
// Maintenance
// ============================================================================

auto ca_store::verify(std::string_view hash) -> ca_result<bool> {
  auto data = get(hash);
  if (!data) {
    return std::unexpected(data.error());
  }

  auto digest = hash::compute(
      hash_algorithm, std::span<const std::uint8_t>(
                          reinterpret_cast<const std::uint8_t*>(data->data()), data->size()));
  std::string computed = digest.to_hex();

  return computed == hash;
}

auto ca_store::verify_all() -> ca_result<std::size_t> {
  std::size_t corrupt_count = 0;

  auto hashes = list_all();
  if (!hashes) {
    return std::unexpected(hashes.error());
  }

  for (const auto& hash : *hashes) {
    auto valid = verify(hash);
    if (!valid) {
      if (valid.error() == ca_error::io_error) {
        return std::unexpected(ca_error::io_error);
      }
      ++corrupt_count;
    } else if (!*valid) {
      ++corrupt_count;
    }
  }

  return corrupt_count;
}

auto ca_store::cleanup_temps() -> ca_result<std::size_t> {
  namespace fs = std::filesystem;
  std::size_t count = 0;

  std::error_code ec;
  for (const auto& shard_entry : fs::directory_iterator(root_, ec)) {
    if (!shard_entry.is_directory()) {
      continue;
    }

    for (const auto& entry : fs::directory_iterator(shard_entry.path(), ec)) {
      if (entry.path().extension() == ".tmp") {
        fs::remove(entry.path(), ec);
        ++count;
      }
    }
  }

  return count;
}

auto ca_store::total_size() -> ca_result<std::uint64_t> {
  namespace fs = std::filesystem;
  std::uint64_t total = 0;

  std::error_code ec;
  for (const auto& shard_entry : fs::directory_iterator(root_, ec)) {
    if (!shard_entry.is_directory()) {
      continue;
    }

    for (const auto& entry : fs::directory_iterator(shard_entry.path(), ec)) {
      if (entry.is_regular_file() && entry.path().extension() != ".tmp") {
        total += entry.file_size(ec);
      }
    }
  }

  return total;
}

auto ca_store::count() -> ca_result<std::size_t> {
  namespace fs = std::filesystem;
  std::size_t cnt = 0;

  std::error_code ec;
  for (const auto& shard_entry : fs::directory_iterator(root_, ec)) {
    if (!shard_entry.is_directory()) {
      continue;
    }

    for (const auto& entry : fs::directory_iterator(shard_entry.path(), ec)) {
      if (entry.is_regular_file() && entry.path().extension() != ".tmp") {
        ++cnt;
      }
    }
  }

  return cnt;
}

auto ca_store::list_all() -> ca_result<std::vector<std::string>> {
  namespace fs = std::filesystem;
  std::vector<std::string> hashes;

  std::error_code ec;
  for (const auto& shard_entry : fs::directory_iterator(root_, ec)) {
    if (!shard_entry.is_directory()) {
      continue;
    }

    for (const auto& entry : fs::directory_iterator(shard_entry.path(), ec)) {
      if (entry.is_regular_file()) {
        auto name = entry.path().filename().string();
        if (name.size() == hash_hex_size && is_valid_hash(name)) {
          hashes.push_back(name);
        }
      }
    }
  }

  return hashes;
}

} // namespace straylight::nix::store
