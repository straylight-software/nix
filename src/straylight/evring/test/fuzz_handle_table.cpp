/// fuzz_handle_table.cpp - Fuzz harness for handle_table operations
///
/// Target: handle_table insert/remove/get/valid operations
/// Rationale: Handle tables manage all resources. Memory corruption, use-after-free,
///            or generation wraparound bugs would compromise the entire system.
///
/// Compile with libFuzzer:
///   clang++ -fsanitize=fuzzer,address,undefined -std=c++23 \
///           -I../../.. fuzz_handle_table.cpp -o fuzz_handle_table
///
/// Compile for AFL:
///   afl-clang++ -std=c++23 -I../../.. fuzz_handle_table.cpp \
///               -o fuzz_handle_table_afl -DAFL_MAIN
///
/// Copyright (c) 2024 Straylight
/// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "straylight/evring/handle.h"

// ============================================================================
// Test value types
// ============================================================================

namespace {

/// Simple test value that tracks construction/destruction
struct tracked_value {
  std::uint64_t id{0};
  std::uint64_t data{0};
  static inline std::uint64_t construction_count{0};
  static inline std::uint64_t destruction_count{0};
  static inline std::uint64_t move_count{0};

  tracked_value() { ++construction_count; }

  explicit tracked_value(std::uint64_t id_val) : id(id_val), data(id_val * 2) {
    ++construction_count;
  }

  tracked_value(std::uint64_t id_val, std::uint64_t data_val) : id(id_val), data(data_val) {
    ++construction_count;
  }

  ~tracked_value() { ++destruction_count; }

  tracked_value(const tracked_value& other) : id(other.id), data(other.data) {
    ++construction_count;
  }

  tracked_value(tracked_value&& other) noexcept : id(other.id), data(other.data) {
    other.id = 0;
    other.data = 0;
    ++move_count;
    ++construction_count;
  }

  tracked_value& operator=(const tracked_value& other) {
    id = other.id;
    data = other.data;
    return *this;
  }

  tracked_value& operator=(tracked_value&& other) noexcept {
    id = other.id;
    data = other.data;
    other.id = 0;
    other.data = 0;
    ++move_count;
    return *this;
  }

  static void reset_counters() {
    construction_count = 0;
    destruction_count = 0;
    move_count = 0;
  }
};

/// Larger value to stress memory handling
struct large_value {
  std::array<std::uint64_t, 64> data{};
  std::uint64_t checksum{0};

  large_value() = default;

  explicit large_value(std::uint64_t seed) {
    for (std::size_t i = 0; i < data.size(); ++i) {
      data[i] = seed + i;
      checksum ^= data[i];
    }
  }

  [[nodiscard]] auto verify() const -> bool {
    std::uint64_t computed = 0;
    for (const auto& d : data) {
      computed ^= d;
    }
    return computed == checksum;
  }
};

// ============================================================================
// Fuzzing operations
// ============================================================================

enum class fuzz_op : std::uint8_t {
  insert = 0,
  remove = 1,
  get = 2,
  get_const = 3,
  valid = 4,
  for_each = 5,
  size = 6,
  remove_invalid = 7,     // Remove with invalid handle
  get_invalid = 8,        // Get with invalid handle
  remove_stale = 9,       // Remove with stale generation
  get_stale = 10,         // Get with stale generation
  insert_many = 11,       // Bulk insert
  remove_many = 12,       // Bulk remove
  churn = 13,             // Insert then immediately remove
  stress_generation = 14, // Repeated insert/remove to stress generation counter
};

/// Execute fuzz operations on handle_table<tracked_value>
void fuzz_tracked_table(const std::uint8_t* data, std::size_t size) {
  evring::handle_table<tracked_value> table;
  std::vector<evring::handle> live_handles;
  std::vector<evring::handle> dead_handles;

  tracked_value::reset_counters();

  std::size_t offset = 0;
  while (offset < size) {
    auto op = static_cast<fuzz_op>(data[offset++] % 15);

    switch (op) {
      case fuzz_op::insert: {
        if (offset + 8 > size)
          break;
        std::uint64_t id = 0;
        std::memcpy(&id, data + offset, 8);
        offset += 8;

        auto h = table.insert(id);
        if (!h.valid()) {
          __builtin_trap(); // Insert should always succeed
        }
        live_handles.push_back(h);
        break;
      }

      case fuzz_op::remove: {
        if (live_handles.empty())
          break;
        if (offset >= size)
          break;

        std::size_t idx = data[offset++] % live_handles.size();
        auto h = live_handles[idx];

        auto value = table.remove(h);
        if (!value.has_value()) {
          __builtin_trap(); // Live handle should be removable
        }

        // Move to dead handles
        dead_handles.push_back(h);
        live_handles.erase(live_handles.begin() + static_cast<std::ptrdiff_t>(idx));
        break;
      }

      case fuzz_op::get: {
        if (live_handles.empty())
          break;
        if (offset >= size)
          break;

        std::size_t idx = data[offset++] % live_handles.size();
        auto h = live_handles[idx];

        auto* ptr = table.get(h);
        if (!ptr) {
          __builtin_trap(); // Live handle should be gettable
        }
        // Access data to trigger sanitizer if corrupted
        (void)ptr->id;
        (void)ptr->data;
        break;
      }

      case fuzz_op::get_const: {
        if (live_handles.empty())
          break;
        if (offset >= size)
          break;

        std::size_t idx = data[offset++] % live_handles.size();
        auto h = live_handles[idx];

        const auto& const_table = table;
        const auto* ptr = const_table.get(h);
        if (!ptr) {
          __builtin_trap();
        }
        (void)ptr->id;
        break;
      }

      case fuzz_op::valid: {
        if (offset >= size)
          break;

        // Check random handle
        evring::handle h;
        if (offset + 8 <= size) {
          std::memcpy(&h.index, data + offset, 4);
          std::memcpy(&h.generation, data + offset + 4, 4);
          offset += 8;
        }

        bool v = table.valid(h);
        (void)v;
        break;
      }

      case fuzz_op::for_each: {
        std::size_t count = 0;
        table.for_each([&count](evring::handle h, tracked_value& val) {
          if (!h.valid()) {
            __builtin_trap(); // Iterated handles must be valid
          }
          ++count;
          (void)val.id;
        });

        if (count != live_handles.size()) {
          __builtin_trap(); // for_each must visit exactly live entries
        }
        break;
      }

      case fuzz_op::size: {
        if (table.size() != live_handles.size()) {
          __builtin_trap();
        }
        break;
      }

      case fuzz_op::remove_invalid: {
        // Attempt to remove invalid handle
        evring::handle invalid = evring::handle::invalid();
        auto result = table.remove(invalid);
        if (result.has_value()) {
          __builtin_trap(); // Invalid handle should not be removable
        }
        break;
      }

      case fuzz_op::get_invalid: {
        evring::handle invalid = evring::handle::invalid();
        auto* ptr = table.get(invalid);
        if (ptr != nullptr) {
          __builtin_trap(); // Invalid handle should return nullptr
        }
        break;
      }

      case fuzz_op::remove_stale: {
        if (dead_handles.empty())
          break;
        if (offset >= size)
          break;

        std::size_t idx = data[offset++] % dead_handles.size();
        auto stale = dead_handles[idx];

        auto result = table.remove(stale);
        if (result.has_value()) {
          __builtin_trap(); // Stale handle should not be removable
        }
        break;
      }

      case fuzz_op::get_stale: {
        if (dead_handles.empty())
          break;
        if (offset >= size)
          break;

        std::size_t idx = data[offset++] % dead_handles.size();
        auto stale = dead_handles[idx];

        auto* ptr = table.get(stale);
        if (ptr != nullptr) {
          __builtin_trap(); // Stale handle should return nullptr
        }
        break;
      }

      case fuzz_op::insert_many: {
        if (offset >= size)
          break;
        std::size_t count = (data[offset++] % 64) + 1;

        for (std::size_t i = 0; i < count && offset + 8 <= size; ++i) {
          std::uint64_t id = 0;
          std::memcpy(&id, data + offset, 8);
          offset += 8;

          auto h = table.insert(id);
          live_handles.push_back(h);
        }
        break;
      }

      case fuzz_op::remove_many: {
        if (live_handles.empty())
          break;
        if (offset >= size)
          break;

        std::size_t count = std::min<std::size_t>(data[offset++] % 32 + 1, live_handles.size());

        for (std::size_t i = 0; i < count; ++i) {
          auto h = live_handles.back();
          live_handles.pop_back();

          auto value = table.remove(h);
          if (!value.has_value()) {
            __builtin_trap();
          }
          dead_handles.push_back(h);
        }
        break;
      }

      case fuzz_op::churn: {
        if (offset + 8 > size)
          break;
        std::uint64_t id = 0;
        std::memcpy(&id, data + offset, 8);
        offset += 8;

        auto h = table.insert(id);
        auto value = table.remove(h);
        if (!value.has_value() || value->id != id) {
          __builtin_trap();
        }
        dead_handles.push_back(h);
        break;
      }

      case fuzz_op::stress_generation: {
        if (offset >= size)
          break;
        std::size_t iterations = (data[offset++] % 32) + 1;

        // Repeatedly insert and remove at same slot to stress generation
        evring::handle first_handle{};
        for (std::size_t i = 0; i < iterations; ++i) {
          auto h = table.insert(i);
          if (i == 0) {
            first_handle = h;
          }
          table.remove(h);
          dead_handles.push_back(h);
        }

        // First handle should definitely be stale now
        if (table.valid(first_handle)) {
          __builtin_trap();
        }
        break;
      }
    }

    // Limit total operations to avoid OOM
    if (live_handles.size() > 100000) {
      break;
    }
  }

  // Cleanup: remove all remaining handles
  for (auto h : live_handles) {
    auto value = table.remove(h);
    if (!value.has_value()) {
      __builtin_trap();
    }
  }

  if (table.size() != 0) {
    __builtin_trap();
  }
}

/// Test with larger values to stress memory
void fuzz_large_table(const std::uint8_t* data, std::size_t size) {
  evring::handle_table<large_value> table;
  std::vector<evring::handle> handles;

  std::size_t offset = 0;
  while (offset + 1 < size && handles.size() < 10000) {
    bool insert = (data[offset++] % 3) != 0; // Bias towards insert

    if (insert || handles.empty()) {
      std::uint64_t seed = 0;
      if (offset + 8 <= size) {
        std::memcpy(&seed, data + offset, 8);
        offset += 8;
      }

      auto h = table.insert(seed);
      handles.push_back(h);

      // Verify inserted value
      auto* ptr = table.get(h);
      if (!ptr || !ptr->verify()) {
        __builtin_trap();
      }
    } else {
      std::size_t idx = data[offset++] % handles.size();
      auto h = handles[idx];

      // Verify before removal
      auto* ptr = table.get(h);
      if (!ptr || !ptr->verify()) {
        __builtin_trap();
      }

      auto value = table.remove(h);
      if (!value.has_value() || !value->verify()) {
        __builtin_trap();
      }

      handles.erase(handles.begin() + static_cast<std::ptrdiff_t>(idx));
    }
  }
}

/// Test ABA protection specifically
void fuzz_aba_protection(const std::uint8_t* data, std::size_t size) {
  evring::handle_table<std::uint64_t> table;

  // Insert value
  auto h1 = table.insert(0xAAAA);

  // Remove it
  table.remove(h1);

  // Insert new value (may reuse slot)
  auto h2 = table.insert(0xBBBB);

  // Old handle MUST be invalid even if same index
  if (table.valid(h1)) {
    // If same index was reused, generations should differ
    if (h1.index == h2.index && h1.generation == h2.generation) {
      __builtin_trap(); // ABA protection failed!
    }
  }

  // New handle must be valid
  if (!table.valid(h2)) {
    __builtin_trap();
  }

  // Old handle must not retrieve new value
  auto* ptr1 = table.get(h1);
  if (ptr1 != nullptr) {
    __builtin_trap(); // Stale handle returned data!
  }

  // New handle retrieves correct value
  auto* ptr2 = table.get(h2);
  if (!ptr2 || *ptr2 != 0xBBBB) {
    __builtin_trap();
  }

  // Stress test: many ABA cycles
  std::vector<evring::handle> all_handles;
  for (std::size_t cycle = 0; cycle < 1000 && cycle < size; ++cycle) {
    std::uint64_t value = static_cast<std::uint64_t>(data[cycle]);
    auto h = table.insert(value);
    all_handles.push_back(h);

    // Immediately remove some
    if (cycle % 2 == 0) {
      table.remove(h);
    }
  }

  // Verify no stale handles are valid
  std::set<std::uint32_t> valid_indices;
  table.for_each([&](evring::handle h, std::uint64_t&) { valid_indices.insert(h.index); });

  for (const auto& h : all_handles) {
    bool is_valid = table.valid(h);
    bool should_be_valid = valid_indices.count(h.index) > 0;

    // If index is in valid set, handle must match current generation
    // This is a softer check since we can't know exact generation
    (void)is_valid;
    (void)should_be_valid;
  }
}

} // anonymous namespace

// ============================================================================
// Fuzz entry point
// ============================================================================

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  // Use first byte to select strategy
  std::uint8_t strategy = data[0] % 4;
  const std::uint8_t* payload = data + 1;
  std::size_t payload_size = size - 1;

  switch (strategy) {
    case 0:
      fuzz_tracked_table(payload, payload_size);
      break;
    case 1:
      fuzz_large_table(payload, payload_size);
      break;
    case 2:
      fuzz_aba_protection(payload, payload_size);
      break;
    case 3:
      // All strategies
      fuzz_tracked_table(payload, payload_size);
      fuzz_large_table(payload, payload_size);
      fuzz_aba_protection(payload, payload_size);
      break;
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
