// straylight // nix-language // runtime
//
// Synchronous I/O Backend Implementation

// Only compile if sync backend is selected
#if defined(STRAYLIGHT_EVAL_IO_SYNC) || defined(STRAYLIGHT_EVAL_IO_ALL)

#  include <cerrno>
#  include <fstream>
#  include <sstream>
#  include <system_error>

#  include "straylight/nix/compiler/runtime/io_backend.h"
#  include "straylight/nix/crypto/hash.h"

namespace straylight::nix::compiler::runtime {

// ─────────────────────────────────────────────────────────────────────────────
// Path Resolution
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Resolve a path relative to the base directory.
auto resolve_path(const std::filesystem::path& base, std::string_view path)
    -> std::filesystem::path {
  std::filesystem::path p(path);
  if (p.is_absolute()) {
    return p;
  }
  return base / p;
}

/// Convert filesystem error code to io_error.
auto map_error(std::error_code ec) -> io_error {
  if (ec == std::errc::no_such_file_or_directory) {
    return io_error::not_found;
  }
  if (ec == std::errc::permission_denied) {
    return io_error::permission_denied;
  }
  if (ec == std::errc::is_a_directory) {
    return io_error::is_directory;
  }
  if (ec == std::errc::not_a_directory) {
    return io_error::not_directory;
  }
  return io_error::io_failed;
}

/// Convert filesystem file_type to our file_type enum.
auto map_file_type(std::filesystem::file_type ft) -> file_type {
  switch (ft) {
    case std::filesystem::file_type::regular:
      return file_type::regular;
    case std::filesystem::file_type::directory:
      return file_type::directory;
    case std::filesystem::file_type::symlink:
      return file_type::symlink;
    default:
      return file_type::unknown;
  }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// io_backend_sync Implementation
// ─────────────────────────────────────────────────────────────────────────────

auto io_backend_sync::read_file(std::string_view path) -> io_result<std::string> {
  auto resolved = resolve_path(base_dir_, path);

  std::error_code ec;
  auto status = std::filesystem::status(resolved, ec);
  if (ec) {
    return std::unexpected(map_error(ec));
  }

  if (status.type() == std::filesystem::file_type::directory) {
    return std::unexpected(io_error::is_directory);
  }

  std::ifstream file(resolved, std::ios::binary | std::ios::ate);
  if (!file) {
    return std::unexpected(io_error::not_found);
  }

  auto size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::string content;
  content.resize(static_cast<std::size_t>(size));

  if (!file.read(content.data(), size)) {
    return std::unexpected(io_error::io_failed);
  }

  return content;
}

auto io_backend_sync::read_dir(std::string_view path) -> io_result<std::vector<dir_entry>> {
  auto resolved = resolve_path(base_dir_, path);

  std::error_code ec;
  auto status = std::filesystem::status(resolved, ec);
  if (ec) {
    return std::unexpected(map_error(ec));
  }

  if (status.type() != std::filesystem::file_type::directory) {
    return std::unexpected(io_error::not_directory);
  }

  std::vector<dir_entry> entries;

  for (const auto& entry : std::filesystem::directory_iterator(resolved, ec)) {
    if (ec) {
      return std::unexpected(map_error(ec));
    }

    dir_entry de;
    de.name = entry.path().filename().string();
    de.type = map_file_type(entry.status().type());
    entries.push_back(std::move(de));
  }

  return entries;
}

auto io_backend_sync::path_exists(std::string_view path) -> bool {
  auto resolved = resolve_path(base_dir_, path);

  std::error_code ec;
  return std::filesystem::exists(resolved, ec) && !ec;
}

auto io_backend_sync::hash_file(std::string_view algo, std::string_view path)
    -> io_result<std::string> {
  // First read the file
  auto content = read_file(path);
  if (!content) {
    return std::unexpected(content.error());
  }

  // Use primitives hash implementation
  using namespace straylight::nix::crypto;

  // Map algo string to hash algorithm
  Algorithm hash_algo;
  if (algo == "sha256") {
    hash_algo = Algorithm::SHA256;
  } else if (algo == "sha512") {
    hash_algo = Algorithm::SHA512;
  } else if (algo == "sha1") {
    hash_algo = Algorithm::SHA1;
  } else if (algo == "md5") {
    hash_algo = Algorithm::MD5;
  } else if (algo == "blake3") {
    hash_algo = Algorithm::BLAKE3;
  } else {
    // Unknown algorithm
    return std::unexpected(io_error::not_supported);
  }

  // Hash the content
  auto hash_result = compute(hash_algo, *content);

  // Return base16 encoded hash
  return hash_result.to_hex();
}

auto io_backend_sync::resolve_import_path(std::string_view path)
    -> io_result<std::filesystem::path> {
  std::filesystem::path p(path);

  // Resolve relative paths against import_base_path_ (set by executor)
  // Falls back to base_dir_ if not set
  std::filesystem::path resolved;
  if (p.is_absolute()) {
    resolved = p;
  } else if (!import_base_path_.empty()) {
    // Relative to the importing file's directory
    resolved = import_base_path_.parent_path() / p;
  } else {
    // Relative to base_dir
    resolved = base_dir_ / p;
  }

  // Canonicalize to resolve . and .. and detect non-existent paths
  std::error_code ec;
  auto canonical = std::filesystem::canonical(resolved, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory) {
      return std::unexpected(io_error::not_found);
    }
    return std::unexpected(io_error::io_failed);
  }

  // If it's a directory, look for default.nix
  if (std::filesystem::is_directory(canonical)) {
    auto default_nix = canonical / "default.nix";
    if (std::filesystem::exists(default_nix)) {
      return default_nix;
    }
    return std::unexpected(io_error::not_found);
  }

  return canonical;
}

auto io_backend_sync::import_file(runtime_context& /*ctx*/, std::string_view path)
    -> io_result<std::int64_t> {
  // 1. Check that we have an import callback
  if (!import_eval_) {
    return std::unexpected(io_error::not_supported);
  }

  // 2. Resolve the path
  auto resolved = resolve_import_path(path);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  auto canonical_path = resolved->string();

  // 3. Check the cache
  auto cache_it = import_cache_.find(canonical_path);
  if (cache_it != import_cache_.end()) {
    return cache_it->second;
  }

  // 4. Check for import cycles
  if (import_in_progress_.contains(canonical_path)) {
    return std::unexpected(io_error::import_cycle);
  }

  // 5. Mark as in-progress
  import_in_progress_.insert(canonical_path);

  // 6. Read the file
  auto content = read_file(canonical_path);
  if (!content) {
    import_in_progress_.erase(canonical_path);
    return std::unexpected(content.error());
  }

  // 7. Save current base path and set new one for nested imports
  auto old_base_path = import_base_path_;
  import_base_path_ = *resolved;

  // 8. Call the import callback (parse → compile → execute)
  std::int64_t result;
  try {
    result = import_eval_(*content, canonical_path);
  } catch (const std::exception& /*e*/) {
    import_base_path_ = old_base_path;
    import_in_progress_.erase(canonical_path);
    return std::unexpected(io_error::eval_error);
  }

  // 9. Restore base path
  import_base_path_ = old_base_path;

  // 10. Remove from in-progress, add to cache
  import_in_progress_.erase(canonical_path);
  import_cache_[canonical_path] = result;

  return result;
}

auto io_backend_sync::derivation(runtime_context& /*ctx*/, std::int64_t /*attrs*/)
    -> io_result<std::int64_t> {
  // TODO: Implement derivation
  // This requires:
  // 1. Extract attrs from nix_value
  // 2. Validate required fields (name, builder, system)
  // 3. Compute drv hash
  // 4. Write .drv file to store
  // 5. Return drv attrset
  //
  // For now, return not_supported until we wire up the store
  return std::unexpected(io_error::not_supported);
}

} // namespace straylight::nix::compiler::runtime

#endif // STRAYLIGHT_EVAL_IO_SYNC
