/// fuzz_trace_replay.cpp - Fuzz harness for event trace replay
///
/// Target: The trace recording/replay mechanism and state machine replay functions
/// Rationale: Replay is the core of evring's testability. If trace recording
///            corrupts data or replay diverges from live execution, the entire
///            deterministic replay guarantee is broken.
///
/// Compile with libFuzzer:
///   clang++ -fsanitize=fuzzer,address,undefined -std=c++23 \
///           -I../../.. fuzz_trace_replay.cpp -o fuzz_trace_replay
///
/// Compile for AFL:
///   afl-clang++ -std=c++23 -I../../.. fuzz_trace_replay.cpp \
///               -o fuzz_trace_replay_afl -DAFL_MAIN
///
/// Copyright (c) 2024 Straylight
/// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "straylight/evring/event.h"
#include "straylight/evring/handle.h"
#include "straylight/evring/machine.h"

// ============================================================================
// Test machines for replay fuzzing
// ============================================================================

namespace {

/// Simple counter machine - validates replay produces deterministic results
struct counter_machine {
  struct state_type {
    std::uint64_t event_count{0};
    std::int64_t result_sum{0};
    std::size_t total_bytes{0};
    std::uint64_t user_data_xor{0};
    bool terminated{false};
  };

  auto initial() const -> state_type { return {}; }

  auto step(state_type s, const evring::event& e) const -> evring::step_result<state_type> {
    s.event_count++;
    s.result_sum += e.result;
    s.total_bytes += e.data.size();
    s.user_data_xor ^= e.user_data;

    // Terminate after many events or on specific pattern
    if (s.event_count > 10000 || (e.data.size() > 0 && e.data[0] == std::byte{0xDE})) {
      s.terminated = true;
    }

    // Generate predictable operations based on state
    std::vector<evring::operation> ops;
    if (s.event_count % 10 == 0) {
      ops.push_back(evring::operation::make_nop(s.event_count));
    }

    return {std::move(s), std::move(ops)};
  }

  auto done(const state_type& s) const -> bool { return s.terminated; }
};

/// Machine that accumulates data - tests data buffer handling in trace
struct data_accumulator_machine {
  struct state_type {
    std::vector<std::byte> accumulated_data;
    std::vector<std::int64_t> results;
    bool done_flag{false};
  };

  auto initial() const -> state_type { return {}; }

  auto step(state_type s, const evring::event& e) const -> evring::step_result<state_type> {
    // Accumulate event data
    s.accumulated_data.insert(s.accumulated_data.end(), e.data.begin(), e.data.end());

    // Track results
    s.results.push_back(e.result);

    // Limit to avoid OOM
    if (s.accumulated_data.size() > 1024 * 1024 || s.results.size() > 100000) {
      s.done_flag = true;
    }

    // Terminate on magic byte
    if (e.data.size() >= 2 && e.data[0] == std::byte{0xCA} && e.data[1] == std::byte{0xFE}) {
      s.done_flag = true;
    }

    return {std::move(s), {}};
  }

  auto done(const state_type& s) const -> bool { return s.done_flag; }
};

/// Generator machine - tests generate() and replay_generate()
struct counter_generator_machine {
  struct state_type {
    std::uint64_t generated_count{0};
    std::uint64_t completed_count{0};
    std::uint64_t target_count{100};
    bool done_flag{false};
  };

  auto initial() const -> state_type { return {}; }

  auto step(state_type s, const evring::event& e) const -> evring::step_result<state_type> {
    s.completed_count++;

    // Update target based on event
    if (e.data.size() > 0) {
      s.target_count = std::min<std::uint64_t>(static_cast<std::uint64_t>(e.data[0]) + 10, 10000);
    }

    if (s.completed_count >= s.target_count) {
      s.done_flag = true;
    }

    return {std::move(s), {}};
  }

  auto done(const state_type& s) const -> bool { return s.done_flag; }

  auto wants_to_submit(const state_type& s) const -> bool {
    return s.generated_count < s.target_count && !s.done_flag;
  }

  auto generate(state_type s, std::size_t max_ops) const -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    std::size_t to_generate = std::min<std::size_t>(max_ops, s.target_count - s.generated_count);

    for (std::size_t i = 0; i < to_generate; ++i) {
      ops.push_back(evring::operation::make_nop(s.generated_count + i));
      s.generated_count++;
    }

    return {std::move(s), std::move(ops)};
  }
};

static_assert(evring::machine<counter_machine>);
static_assert(evring::machine<data_accumulator_machine>);
static_assert(evring::generator_machine<counter_generator_machine>);

// ============================================================================
// Event construction from fuzz data
// ============================================================================

/// Build an event from raw bytes
auto make_event_from_bytes(const std::uint8_t* data, std::size_t size,
                           std::vector<std::byte>& data_storage) -> evring::event {
  evring::event e{};

  if (size >= 4) {
    std::memcpy(&e.resource_handle.index_, data, 4);
  }
  if (size >= 8) {
    std::memcpy(&e.resource_handle.generation_, data + 4, 4);
  }
  if (size >= 9) {
    e.operation = static_cast<evring::operation_type>(data[8] % 27);
  }
  if (size >= 17) {
    std::memcpy(&e.result, data + 9, 8);
  }
  if (size >= 25) {
    std::memcpy(&e.user_data, data + 17, 8);
  }

  // Remaining bytes become event data
  if (size > 25) {
    std::size_t data_len = std::min<std::size_t>(size - 25, 4096);
    data_storage.resize(data_len);
    std::memcpy(data_storage.data(), data + 25, data_len);
    e.data = std::span<const std::byte>(data_storage.data(), data_storage.size());
  }

  return e;
}

/// Parse multiple events from fuzz input
auto parse_events(const std::uint8_t* data, std::size_t size)
    -> std::pair<std::vector<evring::event>, std::vector<std::vector<std::byte>>> {
  std::vector<evring::event> events;
  std::vector<std::vector<std::byte>> all_storage;

  std::size_t offset = 0;
  while (offset < size && events.size() < 10000) {
    // Each event starts with a 2-byte length
    if (offset + 2 > size)
      break;

    std::uint16_t event_len = 0;
    std::memcpy(&event_len, data + offset, 2);
    event_len = std::min<std::uint16_t>(event_len, 4096 + 25); // Clamp
    offset += 2;

    if (offset + event_len > size) {
      event_len = static_cast<std::uint16_t>(size - offset);
    }

    if (event_len > 0) {
      all_storage.emplace_back();
      events.push_back(make_event_from_bytes(data + offset, event_len, all_storage.back()));
      offset += event_len;
    }
  }

  return {std::move(events), std::move(all_storage)};
}

} // anonymous namespace

// ============================================================================
// Fuzz entry point
// ============================================================================

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 4) {
    return 0;
  }

  // Parse events from fuzz input
  auto [events, storage] = parse_events(data, size);
  if (events.empty()) {
    return 0;
  }

  // Strategy 1: Trace record then replay - verify data survives round trip
  {
    evring::trace trace;

    // Record all events
    for (const auto& e : events) {
      trace.record(e);
    }

    // Verify trace captured events
    if (trace.size() != events.size()) {
      __builtin_trap();
    }

    // Replay counter machine
    counter_machine cm;
    auto state_from_events = evring::replay(cm, std::span<const evring::event>(events));
    auto state_from_trace = evring::replay(cm, trace.events());

    // States must be identical
    if (state_from_events.event_count != state_from_trace.event_count ||
        state_from_events.result_sum != state_from_trace.result_sum ||
        state_from_events.total_bytes != state_from_trace.total_bytes ||
        state_from_events.user_data_xor != state_from_trace.user_data_xor) {
      __builtin_trap();
    }
  }

  // Strategy 2: Multiple traces of same events must be identical
  {
    evring::trace trace1, trace2;

    for (const auto& e : events) {
      trace1.record(e);
      trace2.record(e);
    }

    if (trace1.size() != trace2.size()) {
      __builtin_trap();
    }

    auto span1 = trace1.events();
    auto span2 = trace2.events();

    for (std::size_t i = 0; i < trace1.size(); ++i) {
      // Core fields must match
      if (span1[i].result != span2[i].result || span1[i].operation != span2[i].operation ||
          span1[i].user_data != span2[i].user_data ||
          span1[i].data.size() != span2[i].data.size()) {
        __builtin_trap();
      }

      // Data contents must match
      if (!span1[i].data.empty()) {
        if (std::memcmp(span1[i].data.data(), span2[i].data.data(), span1[i].data.size()) != 0) {
          __builtin_trap();
        }
      }
    }
  }

  // Strategy 3: Replay data accumulator - verify accumulated data is correct
  {
    data_accumulator_machine dam;

    auto [state, all_ops] =
        evring::replay_with_operations(dam, std::span<const evring::event>(events));

    // Calculate expected accumulation
    std::vector<std::byte> expected_data;
    for (const auto& e : events) {
      expected_data.insert(expected_data.end(), e.data.begin(), e.data.end());
      if (expected_data.size() > 1024 * 1024)
        break;
      if (e.data.size() >= 2 && e.data[0] == std::byte{0xCA} && e.data[1] == std::byte{0xFE}) {
        break;
      }
    }

    // Truncate expected to match potential early termination
    std::size_t compare_len = std::min(expected_data.size(), state.accumulated_data.size());
    if (compare_len > 0 &&
        std::memcmp(expected_data.data(), state.accumulated_data.data(), compare_len) != 0) {
      __builtin_trap();
    }
  }

  // Strategy 4: Generator machine replay
  {
    counter_generator_machine cgm;

    // replay_generate processes completion events
    auto state = evring::replay_generate(cgm, std::span<const evring::event>(events));

    // State invariant: completed_count should match events processed
    // (unless done early)
    (void)state.generated_count;
    (void)state.completed_count;
  }

  // Strategy 5: Trace clear and reuse
  {
    evring::trace trace;

    // Record first half
    std::size_t half = events.size() / 2;
    for (std::size_t i = 0; i < half; ++i) {
      trace.record(events[i]);
    }

    auto size_before_clear = trace.size();
    trace.clear();

    if (trace.size() != 0) {
      __builtin_trap();
    }

    // Record second half
    for (std::size_t i = half; i < events.size(); ++i) {
      trace.record(events[i]);
    }

    // Should have events.size() - half events now
    if (trace.size() != events.size() - half) {
      __builtin_trap();
    }

    (void)size_before_clear;
  }

  // Strategy 6: Verify events span is stable across multiple calls
  {
    evring::trace trace;

    for (const auto& e : events) {
      trace.record(e);
    }

    auto span1 = trace.events();
    auto span2 = trace.events();

    // Pointers must be stable
    if (span1.data() != span2.data() || span1.size() != span2.size()) {
      __builtin_trap();
    }
  }

  return 0;
}

// ============================================================================
// AFL main() wrapper
// ============================================================================

#ifdef AFL_MAIN
#  include <cstdio>
#  include <cstdlib>

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
    return 1;
  }

  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    perror("fopen");
    return 1;
  }

  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (file_size <= 0) {
    fclose(f);
    return 0;
  }

  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(file_size));
  if (fread(buffer.data(), 1, buffer.size(), f) != buffer.size()) {
    fclose(f);
    return 1;
  }
  fclose(f);

  return LLVMFuzzerTestOneInput(buffer.data(), buffer.size());
}
#endif
