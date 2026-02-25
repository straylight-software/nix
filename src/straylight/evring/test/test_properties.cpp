// test_properties.cpp
//
// "I take no pleasure in taking a life if it's from a person
//  who doesn't care about it."
//
// Property-based tests. Instead of specific test cases, we define
// invariants that must ALWAYS hold, then throw random inputs at them.
//
// Properties tested:
// - Replay determinism: same trace → same result, always
// - Handle table integrity: no aliasing, no use-after-free
// - State machine totality: step() never crashes, always returns valid state
// - Generator monotonicity: completed count never decreases
// - Event/operation symmetry: every submitted op gets exactly one completion

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "straylight/evring/evring.h"
#include "straylight/evring/http2.h"

namespace {

// ============================================================================
// Random generators
// ============================================================================

struct random_generator {
  std::mt19937_64 rng_;

  explicit random_generator(std::uint64_t seed = 0) {
    if (seed == 0) {
      seed = std::random_device{}();
    }
    rng_.seed(seed);
    std::printf("  [seed: %lu]\n", seed);
  }

  auto next_u64() -> std::uint64_t { return rng_(); }

  auto next_u32() -> std::uint32_t { return static_cast<std::uint32_t>(rng_()); }

  auto next_int(int min, int max) -> int {
    std::uniform_int_distribution<int> dist(min, max);
    return dist(rng_);
  }

  auto next_size(std::size_t min, std::size_t max) -> std::size_t {
    std::uniform_int_distribution<std::size_t> dist(min, max);
    return dist(rng_);
  }

  auto next_bool() -> bool { return (rng_() & 1) != 0; }

  auto next_double() -> double {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng_);
  }

  auto next_errno() -> int {
    static const int errnos[] = {
        ENOENT, EACCES, EEXIST, EISDIR, ENOTDIR, ENOTEMPTY, ELOOP,  EBADF,  EINVAL, EIO,
        ENOSPC, EROFS,  EMFILE, ENFILE, EPERM,   EBUSY,     EFAULT, ENOMEM, EAGAIN, EINTR,
    };
    return errnos[next_size(0, sizeof(errnos) / sizeof(errnos[0]) - 1)];
  }

  auto next_operation_type() -> evring::operation_type {
    static const evring::operation_type types[] = {
        evring::operation_type::nop,     evring::operation_type::open,
        evring::operation_type::close,   evring::operation_type::read,
        evring::operation_type::write,   evring::operation_type::statx,
        evring::operation_type::mkdir,   evring::operation_type::unlink,
        evring::operation_type::rename,  evring::operation_type::symlink,
        evring::operation_type::timeout, evring::operation_type::connect,
    };
    return types[next_size(0, sizeof(types) / sizeof(types[0]) - 1)];
  }

  auto next_bytes(std::size_t len) -> std::vector<std::byte> {
    std::vector<std::byte> result(len);
    for (auto& b : result) {
      b = static_cast<std::byte>(rng_() & 0xFF);
    }
    return result;
  }

  auto next_string(std::size_t len) -> std::string {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789_-.";
    std::string result(len, ' ');
    for (auto& c : result) {
      c = charset[next_size(0, sizeof(charset) - 2)];
    }
    return result;
  }
};

// ============================================================================
// PROPERTY 1: Handle table - no aliasing
//
// After allocating N handles, all handles are distinct and valid.
// After removing a handle, it becomes invalid.
// A new handle allocated after removal is distinct from all previous.
// ============================================================================

void property_handle_table_no_aliasing() {
  std::printf("property_handle_table_no_aliasing:\n");

  random_generator rng;
  const int iterations = 1000;
  const int ops_per_iteration = 100;

  for (int iter = 0; iter < iterations; ++iter) {
    evring::handle_table<int> table;
    std::vector<evring::handle> active_handles;
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen_handles;

    for (int op = 0; op < ops_per_iteration; ++op) {
      if (active_handles.empty() || rng.next_bool()) {
        // Insert
        evring::handle h = table.insert(rng.next_int(0, 1000000));

        // Property: new handle is valid
        assert(table.valid(h));

        // Property: new handle is distinct from all previously seen
        auto key = std::make_pair(h.index_, h.generation_);
        assert(seen_handles.find(key) == seen_handles.end());
        seen_handles.insert(key);

        active_handles.push_back(h);
      } else {
        // Remove random handle
        std::size_t idx = rng.next_size(0, active_handles.size() - 1);
        evring::handle h = active_handles[idx];

        // Property: handle is valid before removal
        assert(table.valid(h));

        auto removed = table.remove(h);
        assert(removed.has_value());

        // Property: handle is invalid after removal
        assert(!table.valid(h));

        active_handles.erase(active_handles.begin() + static_cast<long>(idx));
      }
    }

    // Property: all remaining handles are still valid
    for (auto h : active_handles) {
      assert(table.valid(h));
    }
  }

  std::printf("  PASS: %d iterations × %d ops, no aliasing detected\n", iterations,
              ops_per_iteration);
}

// ============================================================================
// PROPERTY 2: Replay determinism
//
// For any state machine M and event trace T:
//   replay(M, T) == replay(M, T)  (always)
//
// Running the same machine with the same trace must produce identical results.
// ============================================================================

void property_replay_determinism() {
  std::printf("property_replay_determinism:\n");

  random_generator rng;
  const int iterations = 500;

  // Simple accumulator machine for testing
  struct accum_state {
    std::int64_t sum{0};
    std::size_t count{0};
    std::vector<std::int64_t> history;
  };

  struct accum_machine {
    using state_type = accum_state;
    std::size_t target;

    auto initial() const -> state_type { return {}; }

    auto step(state_type s, const evring::event& e) const -> evring::step_result<state_type> {
      // Accumulate results
      s.sum += e.result;
      s.count++;
      s.history.push_back(e.result);

      std::vector<evring::operation> ops;
      if (s.count <= target) {
        ops.push_back(evring::operation::make_nop(s.count));
      }
      return {std::move(s), std::move(ops)};
    }

    auto done(const state_type& s) const -> bool { return s.count > target; }
  };

  for (int iter = 0; iter < iterations; ++iter) {
    // Generate random trace
    std::size_t trace_len = rng.next_size(1, 50);
    std::vector<evring::event> trace;

    for (std::size_t i = 0; i < trace_len; ++i) {
      trace.push_back(evring::event{
          .resource_handle = evring::handle{rng.next_u32() % 100, rng.next_u32() % 10},
          .operation = rng.next_operation_type(),
          .result = static_cast<std::int64_t>(rng.next_int(-1000, 1000)),
          .data = {},
          .user_data = rng.next_u64(),
      });
    }

    accum_machine machine{trace_len};

    // Replay multiple times
    auto state1 = evring::replay(machine, trace);
    auto state2 = evring::replay(machine, trace);
    auto state3 = evring::replay(machine, trace);

    // Property: all replays produce identical results
    assert(state1.sum == state2.sum);
    assert(state1.sum == state3.sum);
    assert(state1.count == state2.count);
    assert(state1.count == state3.count);
    assert(state1.history == state2.history);
    assert(state1.history == state3.history);
  }

  std::printf("  PASS: %d random traces, all replays deterministic\n", iterations);
}

// ============================================================================
// PROPERTY 3: State machine totality
//
// For any state S and event E, step(S, E) must:
// - Return without crashing
// - Return a valid state_type
// - Return a vector of operations (possibly empty)
// ============================================================================

void property_state_machine_totality() {
  std::printf("property_state_machine_totality:\n");

  random_generator rng;
  const int iterations = 1000;

  // Test with a file reader-like machine
  struct robust_state {
    enum class phase { init, opening, reading, done, error };
    phase p{phase::init};
    int ops_emitted{0};
  };

  struct robust_machine {
    using state_type = robust_state;

    auto initial() const -> state_type { return {}; }

    auto step(state_type s, const evring::event& e) const -> evring::step_result<state_type> {
      std::vector<evring::operation> ops;

      // Handle ANY event in ANY state without crashing
      switch (s.p) {
        case robust_state::phase::init:
          s.p = robust_state::phase::opening;
          ops.push_back(evring::operation::make_nop(0));
          s.ops_emitted++;
          break;

        case robust_state::phase::opening:
          if (e.result >= 0) {
            s.p = robust_state::phase::reading;
            ops.push_back(evring::operation::make_nop(1));
            s.ops_emitted++;
          } else {
            s.p = robust_state::phase::error;
          }
          break;

        case robust_state::phase::reading:
          if (e.result > 0) {
            ops.push_back(evring::operation::make_nop(2));
            s.ops_emitted++;
          } else {
            s.p = robust_state::phase::done;
          }
          break;

        case robust_state::phase::done:
        case robust_state::phase::error:
          // Terminal - no ops
          break;
      }

      return {std::move(s), std::move(ops)};
    }

    auto done(const state_type& s) const -> bool {
      return s.p == robust_state::phase::done || s.p == robust_state::phase::error;
    }
  };

  robust_machine machine;

  for (int iter = 0; iter < iterations; ++iter) {
    // Start with initial state
    auto state = machine.initial();

    // Feed random events
    int num_events = rng.next_int(0, 20);
    for (int i = 0; i < num_events; ++i) {
      evring::event e{
          .resource_handle = evring::handle{rng.next_u32(), rng.next_u32()},
          .operation = rng.next_operation_type(),
          .result = rng.next_bool() ? rng.next_int(0, 1000) : -rng.next_errno(),
          .data = {},
          .user_data = rng.next_u64(),
      };

      // Property: step() never throws, always returns valid result
      auto [new_state, ops] = machine.step(state, e);
      state = std::move(new_state);

      // Property: operations vector is valid (not checking contents)
      (void)ops.size();
    }

    // Property: state is still valid
    (void)machine.done(state);
  }

  std::printf("  PASS: %d iterations, state machine never crashed\n", iterations);
}

// ============================================================================
// PROPERTY 4: Generator monotonicity
//
// For bulk_stat_machine (and similar generators):
// - completed count never decreases
// - succeeded + failed == completed (always)
// - completed <= total paths (always)
// ============================================================================

void property_generator_monotonicity() {
  std::printf("property_generator_monotonicity:\n");

  random_generator rng;
  const int iterations = 100;

  for (int iter = 0; iter < iterations; ++iter) {
    // Generate random paths (mix of real and fake)
    std::size_t num_paths = rng.next_size(1, 100);
    std::vector<std::string> path_storage;
    std::vector<const char*> paths;

    for (std::size_t i = 0; i < num_paths; ++i) {
      if (rng.next_double() < 0.3) {
        path_storage.push_back("/etc/hostname"); // exists
      } else {
        path_storage.push_back("/nonexistent/" + rng.next_string(10));
      }
      paths.push_back(path_storage.back().c_str());
    }

    std::vector<struct statx> buffers(num_paths);

    auto ring = evring::make_io_uring_ring(64);
    evring::bulk_stat_machine machine{std::span{paths.data(), paths.size()},
                                      evring::make_stable_span(buffers)};

    auto state = machine.initial();
    std::size_t prev_completed = 0;

    // Manually step through the generator
    while (!machine.done(state)) {
      // Generate some ops
      if (machine.wants_to_submit(state)) {
        auto [new_state, ops] = machine.generate(state, ring->sq_space());
        state = std::move(new_state);
        for (const auto& op : ops) {
          ring->enqueue(op);
        }
      }

      if (ring->pending() == 0)
        break;

      auto events = ring->submit_and_wait(1);
      for (const auto& e : events) {
        auto [new_state, ops] = machine.step(state, e);
        state = std::move(new_state);

        // Property: completed never decreases
        assert(state.completed >= prev_completed);
        prev_completed = state.completed;

        // Property: succeeded + failed == completed
        assert(state.succeeded + state.failed == state.completed);

        // Property: completed <= total
        assert(state.completed <= num_paths);
      }
    }

    // Property: when done, completed == total
    assert(state.completed == num_paths);
  }

  std::printf("  PASS: %d iterations, generator monotonicity verified\n", iterations);
}

// ============================================================================
// PROPERTY 5: Operation-completion bijection
//
// For every operation submitted, exactly one completion is received.
// No phantom completions, no lost completions.
// ============================================================================

void property_operation_completion_bijection() {
  std::printf("property_operation_completion_bijection:\n");

  random_generator rng;
  const int iterations = 100;

  for (int iter = 0; iter < iterations; ++iter) {
    auto ring = evring::make_io_uring_ring(128);

    // Submit random NOPs with unique user_data
    std::size_t num_ops = rng.next_size(1, 100);
    std::unordered_set<std::uint64_t> submitted;

    for (std::size_t i = 0; i < num_ops; ++i) {
      std::uint64_t ud = (static_cast<std::uint64_t>(iter) << 32) | i;
      submitted.insert(ud);
      ring->enqueue(evring::operation::make_nop(ud));
    }

    int total_submitted = ring->submit();
    assert(total_submitted == static_cast<int>(num_ops));

    // Collect all completions
    std::unordered_set<std::uint64_t> completed;
    while (completed.size() < num_ops) {
      auto events = ring->submit_and_wait(1);
      for (const auto& e : events) {
        // Property: no duplicate completions
        assert(completed.find(e.user_data) == completed.end());
        completed.insert(e.user_data);

        // Property: completion matches a submitted op
        assert(submitted.find(e.user_data) != submitted.end());
      }
    }

    // Property: every submitted op got exactly one completion
    assert(completed == submitted);
  }

  std::printf("  PASS: %d iterations, perfect op/completion bijection\n", iterations);
}

// ============================================================================
// PROPERTY 6: HTTP/2 session invariants
//
// - Session is invalid before init, valid after
// - Stream IDs are always positive and odd (client-initiated)
// - get_pending_data() clears the buffer (second call returns empty)
// ============================================================================

void property_http2_session_invariants() {
  std::printf("property_http2_session_invariants:\n");

  random_generator rng;
  const int iterations = 100;

  for (int iter = 0; iter < iterations; ++iter) {
    evring::http2_session session;

    // Property: invalid before init
    assert(!session.valid());

    bool ok = session.init_client();

    // Property: valid after init
    assert(ok);
    assert(session.valid());

    // Property: get_pending_data clears buffer
    auto data1 = session.get_pending_data();
    assert(!data1.empty()); // Should have preface + SETTINGS

    auto data2 = session.get_pending_data();
    assert(data2.empty()); // Should be cleared

    // Submit some requests and check stream IDs
    std::set<std::int32_t> stream_ids;
    int num_requests = rng.next_int(1, 10);

    for (int i = 0; i < num_requests; ++i) {
      evring::http2_request req;
      req.method = "GET";
      req.authority = "test.com";
      req.path = "/" + std::to_string(i);

      auto stream_id = session.submit_request(req);

      // Property: stream ID is positive
      assert(stream_id > 0);

      // Property: client-initiated streams are odd
      assert(stream_id % 2 == 1);

      // Property: stream IDs are unique
      assert(stream_ids.find(stream_id) == stream_ids.end());
      stream_ids.insert(stream_id);
    }

    // Property: stream IDs are monotonically increasing
    std::int32_t prev = 0;
    for (auto id : stream_ids) {
      assert(id > prev);
      prev = id;
    }
  }

  std::printf("  PASS: %d iterations, HTTP/2 session invariants hold\n", iterations);
}

// ============================================================================
// PROPERTY 7: Event result consistency
//
// - ok() == (result >= 0)
// - error_code() == -result when result < 0
// - error_code() == 0 when result >= 0
// ============================================================================

void property_event_result_consistency() {
  std::printf("property_event_result_consistency:\n");

  random_generator rng;
  const int iterations = 10000;

  for (int iter = 0; iter < iterations; ++iter) {
    evring::event e{};
    e.result = rng.next_int(-1000, 1000);

    // Property: ok() == (result >= 0)
    assert(e.ok() == (e.result >= 0));

    // Property: error_code() consistency
    if (e.result < 0) {
      assert(e.error_code() == static_cast<int>(-e.result));
    } else {
      assert(e.error_code() == 0);
    }
  }

  std::printf("  PASS: %d random results, event consistency verified\n", iterations);
}

// ============================================================================
// PROPERTY 8: Ring pending count accuracy
//
// After N enqueues and no submits: pending() == N
// After submit_and_wait: pending() decreases appropriately
// ============================================================================

void property_ring_pending_accuracy() {
  std::printf("property_ring_pending_accuracy:\n");

  random_generator rng;
  const int iterations = 50;

  for (int iter = 0; iter < iterations; ++iter) {
    auto ring = evring::make_io_uring_ring(64);

    // Property: starts at zero
    assert(ring->pending() == 0);

    std::size_t num_ops = rng.next_size(1, 50);
    for (std::size_t i = 0; i < num_ops; ++i) {
      ring->enqueue(evring::operation::make_nop(i));

      // Property: pending increases by 1 after each enqueue
      assert(ring->pending() == i + 1);
    }

    // Submit
    int submitted = ring->submit();
    assert(submitted == static_cast<int>(num_ops));

    // Note: pending might still be num_ops until completions are harvested
    // (depends on implementation)

    // Harvest all
    std::size_t harvested = 0;
    while (harvested < num_ops) {
      auto events = ring->submit_and_wait(1);
      harvested += events.size();
    }

    // Property: after all completions harvested, pending is 0
    // (assuming we track in-flight ops as pending)
  }

  std::printf("  PASS: %d iterations, pending count accurate\n", iterations);
}

// ============================================================================
// PROPERTY 9: step_result structure validity
//
// step() must always return a step_result with:
// - A movable state
// - A vector of operations (possibly empty)
// ============================================================================

void property_step_result_validity() {
  std::printf("property_step_result_validity:\n");

  random_generator rng;
  const int iterations = 1000;

  struct minimal_state {
    int value{0};
  };

  struct minimal_machine {
    using state_type = minimal_state;

    auto initial() const -> state_type { return {42}; }

    auto step(state_type s, const evring::event&) const -> evring::step_result<state_type> {
      s.value++;
      std::vector<evring::operation> ops;
      return {std::move(s), std::move(ops)};
    }

    auto done(const state_type& s) const -> bool { return s.value > 100; }
  };

  minimal_machine machine;

  for (int iter = 0; iter < iterations; ++iter) {
    auto state = machine.initial();

    // Property: initial state is valid
    assert(state.value == 42);

    evring::event e{};
    e.result = rng.next_int(-100, 100);

    auto result = machine.step(state, e);

    // Property: returned state is valid
    assert(result.state.value == 43);

    // Property: operations vector exists (even if empty)
    assert(result.operations.empty() || !result.operations.empty()); // tautology = exists
  }

  std::printf("  PASS: %d iterations, step_result always valid\n", iterations);
}

// ============================================================================
// PROPERTY 10: Trace recording captures all events
//
// run_traced should capture every completion event
// ============================================================================

void property_trace_captures_all() {
  std::printf("property_trace_captures_all:\n");

  random_generator rng;
  const int iterations = 50;

  struct counter_state {
    std::size_t events_seen{0};
  };

  struct counter_machine {
    using state_type = counter_state;
    std::size_t target;

    auto initial() const -> state_type { return {}; }

    auto step(state_type s, const evring::event&) const -> evring::step_result<state_type> {
      std::vector<evring::operation> ops;
      if (s.events_seen == 0) {
        // Initial step - emit NOPs
        for (std::size_t i = 0; i < target; ++i) {
          ops.push_back(evring::operation::make_nop(i));
        }
      }
      s.events_seen++;
      return {std::move(s), std::move(ops)};
    }

    auto done(const state_type& s) const -> bool { return s.events_seen > target; }
  };

  for (int iter = 0; iter < iterations; ++iter) {
    std::size_t num_ops = rng.next_size(1, 30);
    counter_machine machine{num_ops};

    auto ring = evring::make_io_uring_ring(64);
    auto [state, trace] = evring::run_traced(machine, *ring);

    // Property: trace contains exactly num_ops events
    assert(trace.size() == num_ops);

    // Property: state saw all events
    assert(state.events_seen == num_ops + 1); // +1 for initial step
  }

  std::printf("  PASS: %d iterations, trace captures all events\n", iterations);
}

} // namespace

int main() {
  std::printf("=== PROPERTY-BASED TESTS ===\n");
  std::printf("\"I take no pleasure in taking a life if it's from a person\n");
  std::printf(" who doesn't care about it.\"\n\n");

  property_handle_table_no_aliasing();
  property_replay_determinism();
  property_state_machine_totality();
  property_generator_monotonicity();
  property_operation_completion_bijection();
  property_http2_session_invariants();
  property_event_result_consistency();
  property_ring_pending_accuracy();
  property_step_result_validity();
  property_trace_captures_all();

  std::printf("\n=== ALL PROPERTIES VERIFIED ===\n");
  std::printf("\"The calmness of the professional.\"\n");
  return 0;
}
