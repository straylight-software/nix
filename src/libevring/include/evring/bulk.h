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

} // namespace evring
