// straylight::nix::primitives::ca_store_machine
//
// evring state machines for async CA store operations.
//
// Provides high-throughput async I/O for content-addressed blob storage:
//   - bulk_ca_has_machine  - Check existence of many blobs
//   - bulk_ca_get_machine  - Read many blobs
//   - ca_put_machine       - Write a single blob atomically
//
// All machines follow the evring pattern: State × Event → State × [Operation]

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <errno.h>
#include <fcntl.h>

#include "straylight/evring/evring.h"
#include "straylight/nix/primitives/ca_store.h"
#include "straylight/nix/primitives/hash.h"

namespace straylight::nix::primitives {

namespace {
/// Check if an event is the initial (empty) event used to trigger first operations.
/// The evring pattern calls step(state, event{}) with a default-constructed event.
[[nodiscard]] inline auto is_initial_event(const evring::event& e) -> bool {
  return !e.resource_handle.valid() && e.operation == evring::operation_type::nop && e.result == 0;
}
} // namespace

// ============================================================================
// bulk_ca_has_machine - Check existence of many blobs via statx
// ============================================================================

/// State for bulk existence checking.
struct bulk_ca_has_state {
  std::size_t next_submit{0}; // Next index to submit
  std::size_t completed{0};   // Number completed
  std::size_t total{0};       // Total count
  bool finished{false};
};

/// Generator machine for checking existence of many CA blobs.
///
/// Note: Uses std::uint8_t for results instead of bool because
/// std::vector<bool> is specialized and doesn't have .data().
///
/// Usage:
///   std::vector<std::string> hashes = {"abc...", "def...", ...};
///   std::vector<std::uint8_t> results(hashes.size());
///
///   bulk_ca_has_machine machine{store_root, hashes, evring::make_stable_span(results)};
///   auto final_state = evring::run_generate(machine, ring);
///   // results[i] is 1 if exists, 0 if not
///
class bulk_ca_has_machine {
public:
  using state_type = bulk_ca_has_state;

  /// Construct with hashes to check and output buffer.
  ///
  /// @param store_root  Root directory of the CA store
  /// @param hashes      Hashes to check (views into caller's storage)
  /// @param results     Output buffer for results (must outlive machine), 1=exists, 0=not
  bulk_ca_has_machine(std::filesystem::path store_root, std::span<const std::string> hashes,
                      evring::stable_span<std::uint8_t> results)
      : root_(std::move(store_root)),
        hashes_(hashes),
        results_(results),
        paths_(hashes.size()),
        statx_bufs_(hashes.size()) {}

  // ─────────────────────────────────────────────────────────────────────────
  // Machine interface
  // ─────────────────────────────────────────────────────────────────────────

  [[nodiscard]] auto initial() const -> state_type {
    return state_type{.next_submit = 0, .completed = 0, .total = hashes_.size(), .finished = false};
  }

  [[nodiscard]] auto step(state_type s, const evring::event& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    if (!is_initial_event(e)) {
      // A statx completed
      auto idx = static_cast<std::size_t>(e.user_data);
      results_[idx] = (e.result >= 0) ? 1 : 0; // File exists if statx succeeded
      s.completed++;

      if (s.completed >= s.total) {
        s.finished = true;
      }
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool { return s.finished; }

  // ─────────────────────────────────────────────────────────────────────────
  // Generator interface
  // ─────────────────────────────────────────────────────────────────────────

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_submit < s.total;
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    while (s.next_submit < s.total && ops.size() < max_ops) {
      auto idx = s.next_submit;
      paths_[idx] = blob_path(hashes_[idx]).string(); // Store for lifetime

      ops.push_back(evring::operation::make_statx(
          AT_FDCWD, paths_[idx].c_str(),
          0,         // flags
          STATX_INO, // minimal mask - just need to know if exists
          evring::make_stable_ref(statx_bufs_[idx]), static_cast<std::uint64_t>(idx)));

      s.next_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

private:
  [[nodiscard]] auto blob_path(std::string_view hash) const -> std::filesystem::path {
    // Shard by first 2 chars
    return root_ / std::string(hash.substr(0, 2)) / std::string(hash);
  }

  std::filesystem::path root_;
  std::span<const std::string> hashes_;
  evring::stable_span<std::uint8_t> results_;
  mutable std::vector<std::string> paths_;
  mutable std::vector<struct statx> statx_bufs_;
};

// ============================================================================
// bulk_ca_read_machine - Read many blobs
// ============================================================================

/// State for bulk blob reading.
struct bulk_ca_read_state {
  enum class phase { opening, reading, closing, done, error };

  phase current_phase{phase::opening};
  std::size_t next_open{0};
  std::size_t next_read{0};
  std::size_t next_close{0};
  std::size_t completed{0};
  std::size_t total{0};
};

/// Result for a single blob read.
struct ca_read_result {
  bool success{false};
  std::vector<std::byte> data;
};

/// Generator machine for reading many CA blobs.
///
/// Note: For simplicity, this uses synchronous reads sized by statx.
/// A production implementation would use pipelined double-buffering.
///
class bulk_ca_read_machine {
public:
  using state_type = bulk_ca_read_state;

  /// Construct with hashes to read and output buffer.
  bulk_ca_read_machine(std::filesystem::path store_root, std::span<const std::string> hashes,
                       evring::stable_span<ca_read_result> results)
      : root_(std::move(store_root)),
        hashes_(hashes),
        results_(results),
        fds_(hashes.size(), -1),
        handles_(hashes.size(), evring::handle::invalid()),
        paths_(hashes.size()),
        sizes_(hashes.size(), 0),
        statx_bufs_(hashes.size()) {}

  [[nodiscard]] auto initial() const -> state_type {
    return state_type{.current_phase = state_type::phase::opening,
                      .next_open = 0,
                      .next_read = 0,
                      .next_close = 0,
                      .completed = 0,
                      .total = hashes_.size()};
  }

  [[nodiscard]] auto step(state_type s, const evring::event& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    if (is_initial_event(e)) {
      return {std::move(s), std::move(ops)};
    }

    auto idx = static_cast<std::size_t>(e.user_data & 0xFFFFFFFF);
    auto op_type = static_cast<std::uint32_t>(e.user_data >> 32);

    switch (op_type) {
      case 0: // statx for size
        if (e.result >= 0) {
          sizes_[idx] = statx_bufs_[idx].stx_size;
          // Now open the file - path already stored in paths_[idx] by generate()
          ops.push_back(evring::operation::make_open(paths_[idx].c_str(), O_RDONLY | O_CLOEXEC, 0,
                                                     (1ULL << 32) | idx));
        } else {
          results_[idx].success = false;
          s.completed++;
        }
        break;

      case 1: // open
        if (e.result >= 0) {
          fds_[idx] = static_cast<int>(e.result);
          handles_[idx] = e.resource_handle;
          // Allocate buffer and read
          results_[idx].data.resize(sizes_[idx]);
          ops.push_back(evring::operation::make_read(
              handles_[idx], evring::make_stable_span(results_[idx].data), 0, (2ULL << 32) | idx));
        } else {
          results_[idx].success = false;
          s.completed++;
        }
        break;

      case 2: // read
        if (e.result >= 0) {
          results_[idx].success = true;
          results_[idx].data.resize(static_cast<std::size_t>(e.result));
        } else {
          results_[idx].success = false;
          results_[idx].data.clear();
        }
        // Close the fd
        if (handles_[idx].valid()) {
          ops.push_back(evring::operation::make_close(handles_[idx], (3ULL << 32) | idx));
          handles_[idx] = evring::handle::invalid();
          fds_[idx] = -1;
        }
        break;

      case 3: // close
        s.completed++;
        if (s.completed >= s.total) {
          s.current_phase = state_type::phase::done;
        }
        break;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.current_phase == state_type::phase::done ||
           s.current_phase == state_type::phase::error;
  }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_open < s.total;
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    while (s.next_open < s.total && ops.size() < max_ops) {
      auto idx = s.next_open;
      // Store path in member vector before using c_str() - path must outlive operation
      paths_[idx] = blob_path(hashes_[idx]).string();

      // First statx to get size
      ops.push_back(evring::operation::make_statx(AT_FDCWD, paths_[idx].c_str(), 0, STATX_SIZE,
                                                  evring::make_stable_ref(statx_bufs_[idx]),
                                                  (0ULL << 32) | idx));

      s.next_open++;
    }

    return {std::move(s), std::move(ops)};
  }

private:
  [[nodiscard]] auto blob_path(std::string_view hash) const -> std::filesystem::path {
    return root_ / std::string(hash.substr(0, 2)) / std::string(hash);
  }

  std::filesystem::path root_;
  std::span<const std::string> hashes_;
  evring::stable_span<ca_read_result> results_;
  mutable std::vector<int> fds_;
  mutable std::vector<evring::handle> handles_;
  mutable std::vector<std::string> paths_;
  mutable std::vector<std::uint64_t> sizes_;
  mutable std::vector<struct statx> statx_bufs_;
};

// ============================================================================
// ca_put_machine - Write a single blob atomically
// ============================================================================

/// State for atomic blob write.
struct ca_put_state {
  enum class phase { opening, writing, syncing, closing, renaming, done, error };

  phase current_phase{phase::opening};
  evring::handle file_handle{evring::handle::invalid()};
  std::size_t written{0};
  std::string hash;
  std::string error_msg;
};

/// Machine for writing a single CA blob atomically.
///
/// Follows the atomic write pattern:
///   1. Open temp file
///   2. Write data
///   3. fsync
///   4. Rename to final path
///
class ca_put_machine {
public:
  using state_type = ca_put_state;

  /// Construct with data to write.
  ///
  /// @param store_root  Root directory of the CA store
  /// @param data        Data to write (hashed to determine path)
  ca_put_machine(std::filesystem::path store_root, std::span<const std::byte> data)
      : root_(std::move(store_root)), data_(data.begin(), data.end()) {
    // Compute hash
    auto digest =
        hash::compute(hash::Algorithm::BLAKE3,
                      std::span<const std::uint8_t>(
                          reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
    hash_ = digest.to_hex();

    // Compute paths
    shard_ = hash_.substr(0, 2);
    final_path_ = root_ / shard_ / hash_;
    temp_path_ = final_path_.string() + ".tmp";
  }

  [[nodiscard]] auto initial() const -> state_type {
    return state_type{.current_phase = state_type::phase::opening,
                      .file_handle = evring::handle::invalid(),
                      .written = 0,
                      .hash = hash_,
                      .error_msg = {}};
  }

  [[nodiscard]] auto step(state_type s, const evring::event& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    if (is_initial_event(e)) {
      // Start by opening temp file
      ops.push_back(evring::operation::make_open(
          temp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644, 0));
      return {std::move(s), std::move(ops)};
    }

    switch (s.current_phase) {
      case state_type::phase::opening:
        if (e.result >= 0) {
          s.file_handle = e.resource_handle;
          s.current_phase = state_type::phase::writing;
          // Write data - make_write takes std::span<const std::byte>
          ops.push_back(evring::operation::make_write(s.file_handle,
                                                      std::span<const std::byte>(data_), 0, 0));
        } else {
          s.current_phase = state_type::phase::error;
          s.error_msg = "Failed to open temp file";
        }
        break;

      case state_type::phase::writing:
        if (e.result >= 0) {
          s.written += static_cast<std::size_t>(e.result);
          if (s.written >= data_.size()) {
            // All written, fsync
            s.current_phase = state_type::phase::syncing;
            ops.push_back(evring::operation::make_fsync(s.file_handle, 0));
          } else {
            // Continue writing
            ops.push_back(evring::operation::make_write(
                s.file_handle, std::span<const std::byte>(data_).subspan(s.written),
                static_cast<std::int64_t>(s.written), 0));
          }
        } else {
          s.current_phase = state_type::phase::error;
          s.error_msg = "Write failed";
          if (s.file_handle.valid()) {
            ops.push_back(evring::operation::make_close(s.file_handle, 0));
            s.file_handle = evring::handle::invalid();
          }
        }
        break;

      case state_type::phase::syncing:
        if (e.result >= 0) {
          // Close before rename
          s.current_phase = state_type::phase::closing;
          ops.push_back(evring::operation::make_close(s.file_handle, 0));
          s.file_handle = evring::handle::invalid();
        } else {
          s.current_phase = state_type::phase::error;
          s.error_msg = "fsync failed";
          if (s.file_handle.valid()) {
            ops.push_back(evring::operation::make_close(s.file_handle, 0));
            s.file_handle = evring::handle::invalid();
          }
        }
        break;

      case state_type::phase::closing:
        // Close completed, now rename
        s.current_phase = state_type::phase::renaming;
        ops.push_back(evring::operation::make_rename(temp_path_.c_str(), final_path_.c_str(), 1));
        break;

      case state_type::phase::renaming:
        // Rename completed
        if (e.result >= 0) {
          s.current_phase = state_type::phase::done;
        } else {
          // ENOENT is ok - another process renamed it
          // EEXIST is ok - content already exists (CA dedup)
          if (e.result == -ENOENT || e.result == -EEXIST) {
            s.current_phase = state_type::phase::done;
          } else {
            s.current_phase = state_type::phase::error;
            s.error_msg = "rename failed";
          }
        }
        break;

      case state_type::phase::done:
      case state_type::phase::error:
        // Terminal states
        break;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.current_phase == state_type::phase::done ||
           s.current_phase == state_type::phase::error;
  }

  /// Get the hash of the data (available after construction).
  [[nodiscard]] auto hash() const -> const std::string& { return hash_; }

private:
  std::filesystem::path root_;
  std::vector<std::byte> data_;
  std::string hash_;
  std::string shard_;
  std::filesystem::path final_path_;
  std::string temp_path_;
};

} // namespace straylight::nix::primitives
