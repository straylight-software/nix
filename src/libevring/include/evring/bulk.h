#pragma once

/// bulk.h - high-performance bulk operations
///
/// These functions bypass the state machine for maximum throughput.
/// Use when you have a known set of operations and don't need
/// fine-grained control over completion handling.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

namespace evring {

// Forward declarations
class ring;

/// result of a bulk operation
struct bulk_result {
  std::size_t succeeded{0};
  std::size_t failed{0};
  std::vector<int> errors; // errno for each failure
};

// ============================================================================
// Bulk file creation
// ============================================================================

/// create many empty files as fast as possible
/// returns number of files successfully created
auto bulk_create_files(ring& ring_instance, std::span<const char* const> paths, mode_t mode = 0644)
    -> bulk_result;

/// create many empty files (string version)
auto bulk_create_files(ring& ring_instance, std::span<const std::string> paths, mode_t mode = 0644)
    -> bulk_result;

// ============================================================================
// Bulk stat
// ============================================================================

/// stat many files, storing results in statx_buffers
/// statx_buffers must have same size as paths
auto bulk_stat(ring& ring_instance, std::span<const char* const> paths,
               std::span<struct statx> statx_buffers, unsigned int mask = STATX_BASIC_STATS)
    -> bulk_result;

/// stat many files (string version)
auto bulk_stat(ring& ring_instance, std::span<const std::string> paths,
               std::span<struct statx> statx_buffers, unsigned int mask = STATX_BASIC_STATS)
    -> bulk_result;

// ============================================================================
// Bulk unlink
// ============================================================================

/// unlink many files
auto bulk_unlink(ring& ring_instance, std::span<const char* const> paths) -> bulk_result;

/// unlink many files (string version)
auto bulk_unlink(ring& ring_instance, std::span<const std::string> paths) -> bulk_result;

// ============================================================================
// File copy
// ============================================================================

/// copy options
struct copy_options {
  std::size_t buffer_size = 1024UL * 1024UL;      // 1 MB default
  std::size_t ring_depth = 32;                    // number of concurrent operations
  bool use_direct_io = false;                     // O_DIRECT for source/dest
  std::function<void(std::uint64_t)> on_progress; // progress callback (bytes copied)
};

/// copy a single file with maximum throughput
/// uses double-buffering and deep SQ queuing
auto copy_file(ring& ring_instance, const char* source, const char* dest,
               copy_options const& options = {}) -> bulk_result;

/// copy a single file (string version)
auto copy_file(ring& ring_instance, std::string const& source, std::string const& dest,
               copy_options const& options = {}) -> bulk_result;

// ============================================================================
// Bulk mkdir
// ============================================================================

/// create many directories
auto bulk_mkdir(ring& ring_instance, std::span<const char* const> paths, mode_t mode = 0755)
    -> bulk_result;

/// create many directories (string version)
auto bulk_mkdir(ring& ring_instance, std::span<const std::string> paths, mode_t mode = 0755)
    -> bulk_result;

// ============================================================================
// Bulk rmdir
// ============================================================================

/// remove many empty directories
auto bulk_rmdir(ring& ring_instance, std::span<const char* const> paths) -> bulk_result;

/// remove many empty directories (string version)
auto bulk_rmdir(ring& ring_instance, std::span<const std::string> paths) -> bulk_result;

// ============================================================================
// Bulk rename
// ============================================================================

/// rename/move many files or directories
/// source_paths[i] is renamed to dest_paths[i]
/// both spans must have the same size
auto bulk_rename(ring& ring_instance, std::span<const char* const> source_paths,
                 std::span<const char* const> dest_paths) -> bulk_result;

/// rename many files (string version)
auto bulk_rename(ring& ring_instance, std::span<const std::string> source_paths,
                 std::span<const std::string> dest_paths) -> bulk_result;

// ============================================================================
// Bulk symlink
// ============================================================================

/// create many symbolic links
/// targets[i] is the target, linkpaths[i] is the symlink path
/// both spans must have the same size
auto bulk_symlink(ring& ring_instance, std::span<const char* const> targets,
                  std::span<const char* const> linkpaths) -> bulk_result;

/// create many symbolic links (string version)
auto bulk_symlink(ring& ring_instance, std::span<const std::string> targets,
                  std::span<const std::string> linkpaths) -> bulk_result;

// ============================================================================
// Bulk link (hard links)
// ============================================================================

/// create many hard links
/// source_paths[i] is linked to dest_paths[i]
/// both spans must have the same size
auto bulk_link(ring& ring_instance, std::span<const char* const> source_paths,
               std::span<const char* const> dest_paths) -> bulk_result;

/// create many hard links (string version)
auto bulk_link(ring& ring_instance, std::span<const std::string> source_paths,
               std::span<const std::string> dest_paths) -> bulk_result;

// ============================================================================
// Bulk readlink
// ============================================================================

/// read many symbolic link targets
/// results are stored in targets buffer (must be pre-sized)
/// each target buffer should be at least PATH_MAX bytes
auto bulk_readlink(ring& ring_instance, std::span<const char* const> linkpaths,
                   std::span<std::string> targets) -> bulk_result;

/// read many symbolic link targets (string version for input)
auto bulk_readlink(ring& ring_instance, std::span<const std::string> linkpaths,
                   std::span<std::string> targets) -> bulk_result;

// ============================================================================
// Recursive directory copy
// ============================================================================

/// options for copy_tree
struct copy_tree_options {
  std::size_t buffer_size = 1024UL * 1024UL;      // buffer size for file copies
  std::size_t ring_depth = 32;                    // concurrent operations per file
  bool preserve_permissions = true;               // preserve file mode
  bool preserve_timestamps = false;               // preserve mtime/atime (not yet implemented)
  bool dereference_symlinks = false;              // copy symlink targets instead of links
  std::function<void(std::uint64_t)> on_progress; // progress callback (bytes copied)
};

/// result of copy_tree operation
struct copy_tree_result {
  std::size_t directories_created{0};
  std::size_t files_copied{0};
  std::size_t symlinks_created{0};
  std::size_t bytes_copied{0};
  std::size_t failed{0};
  std::vector<std::pair<std::string, int>> errors; // path -> errno
};

/// recursively copy a directory tree
/// creates dest directory if it doesn't exist
auto copy_tree(ring& ring_instance, const char* source, const char* dest,
               copy_tree_options const& options = {}) -> copy_tree_result;

/// copy_tree (string version)
auto copy_tree(ring& ring_instance, std::string const& source, std::string const& dest,
               copy_tree_options const& options = {}) -> copy_tree_result;

} // namespace evring
