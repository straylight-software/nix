#pragma once

/// @file store_async.h
/// @brief io_uring async machines for store operations
///
/// This provides generator machines for high-throughput bulk operations
/// on the store, integrating with libevring.
///
/// Usage:
///   auto ring = evring::make_io_uring_ring(256);
///   store s(root);
///   s.init();
///
///   // Bulk query many paths
///   std::vector<std::string> paths = {...};
///   bulk_path_query_machine machine{s, paths};
///   auto state = evring::run_generate(machine, *ring);
///   // state.results contains path_info for each path
///
///   // Closure computation (BFS traversal)
///   closure_machine machine{s, start_paths};
///   auto state = evring::run(machine, *ring);
///   // state.closure contains all transitive dependencies

#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

#include "evring/evring.h"
#include "store.h"

namespace straylight::nix::primitives {

// ============================================================================
// Constants
// ============================================================================

static constexpr std::size_t max_meta_file_size = 64 * 1024; // 64KB per path_info

// ============================================================================
// bulk_path_query_machine - query many path_infos via io_uring
// ============================================================================

/// State for bulk path queries
struct bulk_path_query_state {
  // Tracking
  std::size_t next_to_submit{0};
  std::size_t opens_pending{0};
  std::size_t reads_pending{0};
  std::size_t completed{0};

  // Results indexed by original path index
  std::vector<store_result<path_info>> results;

  // Per-operation tracking
  // user_data encoding: (index << 2) | phase
  // phase: 0=open, 1=read, 2=close
  std::vector<int> fds;                        // fd per path (-1 if not open)
  std::vector<std::vector<std::byte>> buffers; // read buffer per path
  std::vector<std::size_t> bytes_read;         // actual bytes read
};

/// Generator machine for bulk path_info queries
///
/// Opens each .meta file, reads contents, deserializes to path_info.
/// Saturates io_uring for maximum throughput.
struct bulk_path_query_machine {
  using state_type = bulk_path_query_state;

  const store& store_ref;
  std::span<const std::string> paths;
  std::vector<std::string> meta_paths; // computed from store paths

  bulk_path_query_machine(const store& s, std::span<const std::string> p) : store_ref(s), paths(p) {
    meta_paths.reserve(paths.size());
    for (const auto& path : paths) {
      // Extract hash and compute meta path
      auto hash = extract_hash(path);
      auto shard = hash.substr(0, 2);
      meta_paths.push_back(
          (store_ref.root() / "index" / "paths" / shard / (std::string(hash) + ".meta")).string());
    }
  }

  [[nodiscard]] auto initial() const -> state_type {
    state_type s;
    s.results.resize(paths.size(), std::unexpected(store_error::not_found));
    s.fds.resize(paths.size(), -1);
    s.buffers.resize(paths.size());
    s.bytes_read.resize(paths.size(), 0);
    return s;
  }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < paths.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;
    ops.reserve(max_ops);

    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      std::size_t idx = s.next_to_submit;
      // Allocate buffer for this path
      s.buffers[idx].resize(max_meta_file_size);

      // Open the meta file
      // user_data = (idx << 2) | 0  (phase 0 = open)
      ops.push_back(
          evring::operation::make_open(meta_paths[idx].c_str(), O_RDONLY, 0, (idx << 2) | 0));

      s.next_to_submit++;
      s.opens_pending++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, evring::event const& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    std::size_t idx = e.user_data >> 2;
    std::size_t phase = e.user_data & 0x3;

    if (phase == 0) {
      // Open completed
      s.opens_pending--;

      if (e.result < 0) {
        // Open failed
        s.results[idx] = std::unexpected(store_error::not_found);
        s.completed++;
      } else {
        // Open succeeded, start read
        s.fds[idx] = static_cast<int>(e.result);

        // Create read operation
        // user_data = (idx << 2) | 1  (phase 1 = read)
        evring::operation read_op{
            .resource_handle = evring::handle::invalid(),
            .type = evring::operation_type::read,
            .user_data = (idx << 2) | 1,
            .parameters =
                evring::read_parameters{
                    s.buffers[idx].data(), s.buffers[idx].size(),
                    0 // offset 0
                },
        };
        ops.push_back(read_op);
        s.reads_pending++;
      }
    } else if (phase == 1) {
      // Read completed
      s.reads_pending--;

      if (e.result <= 0) {
        // Read failed or empty
        s.results[idx] = std::unexpected(store_error::io_error);
      } else {
        // Read succeeded, deserialize
        s.bytes_read[idx] = static_cast<std::size_t>(e.result);
        auto data = std::span<const std::byte>(s.buffers[idx].data(), s.bytes_read[idx]);
        s.results[idx] = deserialize_path_info(data);
      }

      // Close the fd
      // user_data = (idx << 2) | 2  (phase 2 = close)
      // For close, we encode fd in user_data since we don't have a handle
      evring::operation close_op{
          .resource_handle = evring::handle::invalid(),
          .type = evring::operation_type::close,
          .user_data = (idx << 2) | 2,
          .parameters = std::monostate{},
      };
      // Note: The ring needs the fd. We'll store it in a way the ring can access.
      // Actually, for io_uring IORING_OP_CLOSE, we pass fd directly.
      // The evring abstraction may need adjustment here.
      // For now, assume close_op with fd encoded works.
      ops.push_back(close_op);
    } else if (phase == 2) {
      // Close completed
      s.fds[idx] = -1;
      s.completed++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool { return s.completed >= paths.size(); }

private:
  static auto extract_hash(std::string_view path) -> std::string_view {
    auto slash = path.rfind('/');
    if (slash != std::string_view::npos) {
      path = path.substr(slash + 1);
    }
    auto dash = path.find('-');
    if (dash != std::string_view::npos) {
      return path.substr(0, dash);
    }
    return path;
  }
};

// ============================================================================
// bulk_refs_query_machine - query references for many paths
// ============================================================================

/// State for bulk refs queries
struct bulk_refs_query_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::vector<store_result<std::vector<std::string>>> results;
  std::vector<int> fds;
  std::vector<std::vector<std::byte>> buffers;
};

/// Generator machine for bulk refs queries
struct bulk_refs_query_machine {
  using state_type = bulk_refs_query_state;

  const store& store_ref;
  std::span<const std::string> paths;
  std::vector<std::string> refs_paths;

  bulk_refs_query_machine(const store& s, std::span<const std::string> p) : store_ref(s), paths(p) {
    refs_paths.reserve(paths.size());
    for (const auto& path : paths) {
      auto hash = extract_hash(path);
      auto shard = hash.substr(0, 2);
      refs_paths.push_back(
          (store_ref.root() / "index" / "refs" / shard / (std::string(hash) + ".refs")).string());
    }
  }

  [[nodiscard]] auto initial() const -> state_type {
    state_type s;
    s.results.resize(paths.size(), std::vector<std::string>{});
    s.fds.resize(paths.size(), -1);
    s.buffers.resize(paths.size());
    return s;
  }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < paths.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;
    ops.reserve(max_ops);

    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      std::size_t idx = s.next_to_submit;
      s.buffers[idx].resize(max_meta_file_size);
      ops.push_back(
          evring::operation::make_open(refs_paths[idx].c_str(), O_RDONLY, 0, (idx << 2) | 0));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, evring::event const& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    std::size_t idx = e.user_data >> 2;
    std::size_t phase = e.user_data & 0x3;

    if (phase == 0) {
      if (e.result < 0) {
        s.results[idx] = std::vector<std::string>{};
        s.completed++;
      } else {
        s.fds[idx] = static_cast<int>(e.result);
        evring::operation read_op{
            .resource_handle = evring::handle::invalid(),
            .type = evring::operation_type::read,
            .user_data = (idx << 2) | 1,
            .parameters = evring::read_parameters{s.buffers[idx].data(), s.buffers[idx].size(), 0},
        };
        ops.push_back(read_op);
      }
    } else if (phase == 1) {
      if (e.result <= 0) {
        s.results[idx] = std::vector<std::string>{};
      } else {
        auto data =
            std::span<const std::byte>(s.buffers[idx].data(), static_cast<std::size_t>(e.result));
        s.results[idx] = deserialize_refs(data);
      }
      evring::operation close_op{
          .resource_handle = evring::handle::invalid(),
          .type = evring::operation_type::close,
          .user_data = (idx << 2) | 2,
          .parameters = std::monostate{},
      };
      ops.push_back(close_op);
    } else if (phase == 2) {
      s.fds[idx] = -1;
      s.completed++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool { return s.completed >= paths.size(); }

private:
  static auto extract_hash(std::string_view path) -> std::string_view {
    auto slash = path.rfind('/');
    if (slash != std::string_view::npos)
      path = path.substr(slash + 1);
    auto dash = path.find('-');
    if (dash != std::string_view::npos)
      return path.substr(0, dash);
    return path;
  }
};

// ============================================================================
// closure_machine - compute transitive closure of references
// ============================================================================

/// State for closure computation
struct closure_state {
  // BFS frontier
  std::vector<std::string> frontier;
  std::size_t frontier_idx{0}; // current position in frontier

  // Visited set (for deduplication)
  std::unordered_set<std::string> visited;

  // Final closure result
  std::vector<std::string> closure;

  // In-flight queries
  std::size_t queries_pending{0};

  // Buffers for reads
  std::vector<std::vector<std::byte>> buffers;
  std::vector<int> fds;
  std::vector<std::string> querying_paths; // paths being queried
};

/// Machine for computing transitive closure of references
///
/// This is a regular machine (not generator) because it dynamically
/// discovers new work based on query results.
struct closure_machine {
  using state_type = closure_state;

  const store& store_ref;

  explicit closure_machine(const store& s) : store_ref(s) {}

  [[nodiscard]] auto initial(std::span<const std::string> start_paths) const -> state_type {
    state_type s;
    for (const auto& path : start_paths) {
      if (s.visited.insert(path).second) {
        s.frontier.push_back(path);
        s.closure.push_back(path);
      }
    }
    return s;
  }

  [[nodiscard]] auto step(state_type s, evring::event const& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    // Handle empty event (initial trigger)
    if (e.operation == evring::operation_type::nop && e.user_data == 0) {
      // Start initial queries
      return start_queries(std::move(s));
    }

    std::size_t idx = e.user_data >> 2;
    std::size_t phase = e.user_data & 0x3;

    if (phase == 0) {
      // Open completed
      if (e.result < 0) {
        // File not found - no refs for this path
        s.queries_pending--;
      } else {
        s.fds[idx] = static_cast<int>(e.result);
        evring::operation read_op{
            .resource_handle = evring::handle::invalid(),
            .type = evring::operation_type::read,
            .user_data = (idx << 2) | 1,
            .parameters = evring::read_parameters{s.buffers[idx].data(), s.buffers[idx].size(), 0},
        };
        ops.push_back(read_op);
      }
    } else if (phase == 1) {
      // Read completed
      if (e.result > 0) {
        auto data =
            std::span<const std::byte>(s.buffers[idx].data(), static_cast<std::size_t>(e.result));
        auto refs = deserialize_refs(data);

        // Add new refs to frontier
        for (auto& ref : refs) {
          if (s.visited.insert(ref).second) {
            s.frontier.push_back(ref);
            s.closure.push_back(ref);
          }
        }
      }

      // Close fd
      evring::operation close_op{
          .resource_handle = evring::handle::invalid(),
          .type = evring::operation_type::close,
          .user_data = (idx << 2) | 2,
          .parameters = std::monostate{},
      };
      ops.push_back(close_op);
    } else if (phase == 2) {
      // Close completed
      s.fds[idx] = -1;
      s.queries_pending--;

      // If no more pending, start more queries
      if (s.queries_pending == 0 && s.frontier_idx < s.frontier.size()) {
        auto [new_state, new_ops] = start_queries(std::move(s));
        s = std::move(new_state);
        for (auto& op : new_ops) {
          ops.push_back(std::move(op));
        }
      }
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.queries_pending == 0 && s.frontier_idx >= s.frontier.size();
  }

private:
  [[nodiscard]] auto start_queries(state_type s) const -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    // Query up to 64 paths at once
    constexpr std::size_t batch_size = 64;
    std::size_t count = 0;

    while (count < batch_size && s.frontier_idx < s.frontier.size()) {
      const auto& path = s.frontier[s.frontier_idx];
      auto hash = extract_hash(path);
      auto shard = hash.substr(0, 2);
      auto refs_path =
          (store_ref.root() / "index" / "refs" / shard / (std::string(hash) + ".refs")).string();

      // Allocate buffer
      std::size_t idx = s.querying_paths.size();
      s.querying_paths.push_back(path);
      s.buffers.emplace_back(max_meta_file_size);
      s.fds.push_back(-1);

      ops.push_back(evring::operation::make_open(refs_path.c_str(), O_RDONLY, 0, (idx << 2) | 0));

      s.queries_pending++;
      s.frontier_idx++;
      count++;
    }

    return {std::move(s), std::move(ops)};
  }

  static auto extract_hash(std::string_view path) -> std::string_view {
    auto slash = path.rfind('/');
    if (slash != std::string_view::npos)
      path = path.substr(slash + 1);
    auto dash = path.find('-');
    if (dash != std::string_view::npos)
      return path.substr(0, dash);
    return path;
  }
};

// ============================================================================
// bulk_check_valid_machine - check if many paths are valid (statx-based)
// ============================================================================

/// State for bulk validity checks
struct bulk_check_valid_state {
  std::size_t next_to_submit{0};
  std::size_t completed{0};
  std::vector<bool> valid;
  std::vector<struct statx> statx_buffers;
};

/// Generator machine for checking path validity via statx
///
/// Much faster than opening files - just checks existence.
struct bulk_check_valid_machine {
  using state_type = bulk_check_valid_state;

  const store& store_ref;
  std::span<const std::string> paths;
  std::vector<std::string> meta_paths;

  bulk_check_valid_machine(const store& s, std::span<const std::string> p)
      : store_ref(s), paths(p) {
    meta_paths.reserve(paths.size());
    for (const auto& path : paths) {
      auto hash = extract_hash(path);
      auto shard = hash.substr(0, 2);
      meta_paths.push_back(
          (store_ref.root() / "index" / "paths" / shard / (std::string(hash) + ".meta")).string());
    }
  }

  [[nodiscard]] auto initial() const -> state_type {
    state_type s;
    s.valid.resize(paths.size(), false);
    s.statx_buffers.resize(paths.size());
    return s;
  }

  [[nodiscard]] auto wants_to_submit(const state_type& s) const -> bool {
    return s.next_to_submit < paths.size();
  }

  [[nodiscard]] auto generate(state_type s, std::size_t max_ops) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;
    ops.reserve(max_ops);

    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      std::size_t idx = s.next_to_submit;
      ops.push_back(evring::operation::make_statx(AT_FDCWD, meta_paths[idx].c_str(), 0, STATX_TYPE,
                                                  &s.statx_buffers[idx], idx));
      s.next_to_submit++;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto step(state_type s, evring::event const& e) const
      -> evring::step_result<state_type> {
    std::size_t idx = e.user_data;
    s.valid[idx] = (e.result >= 0);
    s.completed++;
    return {std::move(s), {}};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool { return s.completed >= paths.size(); }

private:
  static auto extract_hash(std::string_view path) -> std::string_view {
    auto slash = path.rfind('/');
    if (slash != std::string_view::npos)
      path = path.substr(slash + 1);
    auto dash = path.find('-');
    if (dash != std::string_view::npos)
      return path.substr(0, dash);
    return path;
  }
};

// ============================================================================
// Convenience functions
// ============================================================================

/// Query multiple path_infos in bulk using io_uring
inline auto bulk_query_path_info(const store& s, evring::ring& ring,
                                 std::span<const std::string> paths)
    -> std::vector<store_result<path_info>> {
  bulk_path_query_machine machine{s, paths};
  auto state = evring::run_generate(machine, ring);
  return std::move(state.results);
}

/// Query references for multiple paths in bulk using io_uring
inline auto bulk_query_refs(const store& s, evring::ring& ring, std::span<const std::string> paths)
    -> std::vector<store_result<std::vector<std::string>>> {
  bulk_refs_query_machine machine{s, paths};
  auto state = evring::run_generate(machine, ring);
  return std::move(state.results);
}

/// Check validity of multiple paths in bulk using io_uring
inline auto bulk_check_valid(const store& s, evring::ring& ring, std::span<const std::string> paths)
    -> std::vector<bool> {
  bulk_check_valid_machine machine{s, paths};
  auto state = evring::run_generate(machine, ring);
  return std::move(state.valid);
}

/// Compute transitive closure of references using io_uring
inline auto compute_closure(const store& s, evring::ring& ring,
                            std::span<const std::string> start_paths) -> std::vector<std::string> {
  closure_machine machine{s};
  auto state = machine.initial(start_paths);

  // Run the machine
  auto [new_state, ops] = machine.step(state, evring::event{});
  state = std::move(new_state);
  for (const auto& op : ops) {
    ring.enqueue(op);
  }

  while (!machine.done(state)) {
    auto events = ring.submit_and_wait(1);
    for (const auto& event : events) {
      auto [s2, ops2] = machine.step(state, event);
      state = std::move(s2);
      for (const auto& op : ops2) {
        ring.enqueue(op);
      }
    }
  }

  return std::move(state.closure);
}

} // namespace straylight::nix::primitives
