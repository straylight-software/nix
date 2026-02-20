/// fuzz_state_machine.cpp - Fuzz harness for state machine step() function
///
/// Target: The core state machine transition logic
/// Rationale: step() is the heart of evring. If it can be crashed or made to
///            exhibit undefined behavior, the entire system fails.
///
/// Compile with libFuzzer:
///   clang++ -fsanitize=fuzzer,address,undefined -std=c++23 \
///           -I../../.. fuzz_state_machine.cpp -o fuzz_state_machine
///
/// Compile for AFL:
///   afl-clang++ -std=c++23 -I../../.. fuzz_state_machine.cpp \
///               -o fuzz_state_machine_afl -DAFL_MAIN
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
// Test state machine - designed to exercise all code paths
// ============================================================================

namespace {

/// A deliberately complex state machine that exercises edge cases
/// based on fuzzed event data
struct fuzz_test_machine {
  struct state_type {
    std::uint64_t transition_count{0};
    std::uint64_t accumulated_result{0};
    std::size_t total_data_bytes{0};
    evring::handle last_handle{};
    evring::operation_type last_op_type{evring::operation_type::nop};
    bool saw_negative_result{false};
    bool saw_zero_data{false};
    bool saw_large_data{false};
    bool done_flag{false};
  };

  auto initial() const -> state_type { return {}; }

  auto step(state_type s, const evring::event& e) const -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    s.transition_count++;
    s.accumulated_result += static_cast<std::uint64_t>(
        e.result >= 0 ? e.result : -e.result); // Avoid overflow on large negative
    s.total_data_bytes += e.data.size();
    s.last_handle = e.resource_handle;
    s.last_op_type = e.operation;

    if (e.result < 0) {
      s.saw_negative_result = true;
    }
    if (e.data.empty()) {
      s.saw_zero_data = true;
    }
    if (e.data.size() > 4096) {
      s.saw_large_data = true;
    }

    // Generate operations based on fuzzed data
    // This tests that step_result handles varying operation counts

    // Use event data to determine operation generation
    std::size_t num_ops = 0;
    if (!e.data.empty()) {
      num_ops = static_cast<std::size_t>(e.data[0]) % 16; // 0-15 operations
    }

    for (std::size_t i = 0; i < num_ops; ++i) {
      // Vary operation type based on accumulated state
      switch ((s.transition_count + i) % 8) {
        case 0:
          ops.push_back(evring::operation::make_nop(e.user_data + i));
          break;
        case 1:
          ops.push_back(evring::operation::make_timeout(1000000 * (i + 1), e.user_data + i));
          break;
        case 2:
          if (e.resource_handle.valid()) {
            // Use a stack buffer for fuzz testing - we won't actually execute these
            // Size is 0 so no actual read will occur
            std::byte dummy_buf[1];
            ops.push_back(evring::operation::make_read(
                e.resource_handle, evring::make_stable_span(std::span<std::byte>(dummy_buf, 0)), -1,
                e.user_data + i));
          }
          break;
        case 3:
          if (e.resource_handle.valid()) {
            ops.push_back(evring::operation::make_close(e.resource_handle, e.user_data + i));
          }
          break;
        case 4:
          ops.push_back(evring::operation::make_socket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0, 0,
                                                       e.user_data + i));
          break;
        case 5:
          if (e.resource_handle.valid()) {
            ops.push_back(evring::operation::make_shutdown(e.resource_handle, 2, e.user_data + i));
          }
          break;
        case 6:
          ops.push_back(evring::operation::make_cancel(e.resource_handle, e.user_data + i));
          break;
        default:
          ops.push_back(evring::operation::make_nop(e.user_data + i));
          break;
      }
    }

    // Termination condition based on fuzzed data
    if (s.transition_count >= 1000 || (e.data.size() > 1 && e.data[1] == std::byte{0xFF})) {
      s.done_flag = true;
    }

    return {std::move(s), std::move(ops)};
  }

  auto done(const state_type& s) const -> bool { return s.done_flag; }
};

// Verify our machine satisfies the concept
static_assert(evring::machine<fuzz_test_machine>);

// ============================================================================
// Event deserialization from fuzz input
// ============================================================================

/// Deserialize a single event from raw bytes
/// Returns nullopt if not enough bytes
auto deserialize_event(const std::uint8_t* data, std::size_t size, std::size_t& consumed)
    -> std::optional<evring::event> {
  // Minimum event header: handle(8) + op_type(1) + result(8) + user_data(8) + data_len(2) = 27
  constexpr std::size_t kMinHeader = 27;
  if (size < kMinHeader) {
    return std::nullopt;
  }

  evring::event e{};

  // Parse handle
  std::memcpy(&e.resource_handle.index_, data, 4);
  std::memcpy(&e.resource_handle.generation_, data + 4, 4);

  // Parse operation type (clamped to valid range)
  e.operation = static_cast<evring::operation_type>(data[8] % 27); // 27 operation types

  // Parse result
  std::memcpy(&e.result, data + 9, 8);

  // Parse user_data
  std::memcpy(&e.user_data, data + 17, 8);

  // Parse data length (16-bit to avoid massive allocations)
  std::uint16_t data_len = 0;
  std::memcpy(&data_len, data + 25, 2);

  // Clamp data length to available bytes and reasonable maximum
  data_len = std::min<std::uint16_t>(data_len, 8192);
  std::size_t available_data = size - kMinHeader;
  data_len = std::min<std::uint16_t>(data_len, static_cast<std::uint16_t>(available_data));

  consumed = kMinHeader + data_len;

  // Note: e.data will be set by caller from buffer
  // e.statx_buffer is left null

  return e;
}

/// Deserialize multiple events from fuzz input
/// The event data spans are valid only as long as data buffer is valid
auto deserialize_events(const std::uint8_t* data, std::size_t size)
    -> std::pair<std::vector<evring::event>, std::vector<std::vector<std::byte>>> {
  std::vector<evring::event> events;
  std::vector<std::vector<std::byte>> data_storage;

  std::size_t offset = 0;
  while (offset < size) {
    std::size_t consumed = 0;
    auto maybe_event = deserialize_event(data + offset, size - offset, consumed);
    if (!maybe_event) {
      break;
    }

    // Extract data portion
    constexpr std::size_t kMinHeader = 27;
    std::uint16_t data_len = 0;
    std::memcpy(&data_len, data + offset + 25, 2);
    data_len = std::min<std::uint16_t>(data_len, 8192);
    std::size_t available_data = size - offset - kMinHeader;
    data_len = std::min<std::uint16_t>(data_len, static_cast<std::uint16_t>(available_data));

    if (data_len > 0) {
      std::vector<std::byte> event_data(data_len);
      std::memcpy(event_data.data(), data + offset + kMinHeader, data_len);
      data_storage.push_back(std::move(event_data));
      maybe_event->data =
          std::span<const std::byte>(data_storage.back().data(), data_storage.back().size());
    }

    events.push_back(*maybe_event);
    offset += consumed;
  }

  return {std::move(events), std::move(data_storage)};
}

} // anonymous namespace

// ============================================================================
// Fuzz entry point
// ============================================================================

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Avoid trivially empty inputs
  if (size < 27) {
    return 0;
  }

  // Deserialize events from fuzz input
  auto [events, storage] = deserialize_events(data, size);

  if (events.empty()) {
    return 0;
  }

  // Strategy 1: Direct step() calls
  {
    fuzz_test_machine machine;
    auto state = machine.initial();

    for (const auto& e : events) {
      auto result = machine.step(state, e);
      state = std::move(result.state);

      // Validate result invariants
      // Operations vector must be well-formed (not corrupted)
      for (const auto& op : result.operations) {
        // Access fields to trigger sanitizer if corrupted
        (void)op.type;
        (void)op.resource_handle.valid();
        (void)op.user_data;
      }

      if (machine.done(state)) {
        break;
      }
    }
  }

  // Strategy 2: Use replay() function
  {
    fuzz_test_machine machine;
    auto final_state = evring::replay(machine, std::span<const evring::event>(events));

    // Validate final state is consistent
    (void)final_state.transition_count;
    (void)final_state.accumulated_result;
    (void)final_state.saw_negative_result;
  }

  // Strategy 3: Use replay_with_operations() function
  {
    fuzz_test_machine machine;
    auto [final_state, all_ops] =
        evring::replay_with_operations(machine, std::span<const evring::event>(events));

    // Validate operations captured correctly
    for (const auto& ops_batch : all_ops) {
      for (const auto& op : ops_batch) {
        (void)op.type;
        (void)op.resource_handle.valid();
      }
    }
  }

  // Strategy 4: Trace recording and replay
  {
    evring::trace trace;

    // Record events
    for (const auto& e : events) {
      trace.record(e);
    }

    // Validate trace size
    if (trace.size() != events.size()) {
      __builtin_trap(); // Should never happen
    }

    // Replay from trace
    fuzz_test_machine machine;
    auto state = evring::replay(machine, trace.events());

    // Clear and re-record
    trace.clear();
    if (trace.size() != 0) {
      __builtin_trap(); // Should never happen
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
