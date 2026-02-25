// straylight::nix::data::tests
//
// Tests for LRU cache primitives.
// Unit tests and property-based tests.

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "../lru_cache.h"

namespace data = straylight::nix::data;

// ─────────────────────────────────────────────────────────────────────────────
// Basic functionality tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LRUCache basic put and get", "[lru][basic]") {
  data::LRUCache<std::string, int> cache(3);

  REQUIRE(cache.capacity() == 3);
  REQUIRE(cache.size() == 0);
  REQUIRE(cache.empty());

  cache.put("a", 1);
  cache.put("b", 2);
  cache.put("c", 3);

  REQUIRE(cache.size() == 3);
  REQUIRE(cache.full());

  REQUIRE(cache.get("a") == 1);
  REQUIRE(cache.get("b") == 2);
  REQUIRE(cache.get("c") == 3);
  REQUIRE(cache.get("d") == std::nullopt);
}

TEST_CASE("LRUCache eviction on overflow", "[lru][eviction]") {
  data::LRUCache<std::string, int> cache(2);

  cache.put("a", 1);
  cache.put("b", 2);

  // "a" is LRU, should be evicted
  cache.put("c", 3);

  REQUIRE(cache.size() == 2);
  REQUIRE(cache.get("a") == std::nullopt); // evicted
  REQUIRE(cache.get("b") == 2);
  REQUIRE(cache.get("c") == 3);
}

TEST_CASE("LRUCache access promotes entry", "[lru][promotion]") {
  data::LRUCache<std::string, int> cache(2);

  cache.put("a", 1);
  cache.put("b", 2);

  // Access "a" to make it most recently used
  (void)cache.get("a");

  // Now "b" is LRU, should be evicted
  cache.put("c", 3);

  REQUIRE(cache.get("a") == 1);            // still present
  REQUIRE(cache.get("b") == std::nullopt); // evicted
  REQUIRE(cache.get("c") == 3);
}

TEST_CASE("LRUCache update existing key", "[lru][update]") {
  data::LRUCache<std::string, int> cache(2);

  cache.put("a", 1);
  cache.put("a", 10);

  REQUIRE(cache.size() == 1);
  REQUIRE(cache.get("a") == 10);
}

TEST_CASE("LRUCache erase", "[lru][erase]") {
  data::LRUCache<std::string, int> cache(3);

  cache.put("a", 1);
  cache.put("b", 2);
  cache.put("c", 3);

  REQUIRE(cache.erase("b"));
  REQUIRE(cache.size() == 2);
  REQUIRE(cache.get("b") == std::nullopt);

  REQUIRE_FALSE(cache.erase("nonexistent"));
}

TEST_CASE("LRUCache clear", "[lru][clear]") {
  data::LRUCache<std::string, int> cache(3);

  cache.put("a", 1);
  cache.put("b", 2);

  cache.clear();

  REQUIRE(cache.size() == 0);
  REQUIRE(cache.empty());
  REQUIRE(cache.get("a") == std::nullopt);
}

TEST_CASE("LRUCache contains", "[lru][contains]") {
  data::LRUCache<std::string, int> cache(3);

  cache.put("a", 1);

  REQUIRE(cache.contains("a"));
  REQUIRE_FALSE(cache.contains("b"));
}

TEST_CASE("LRUCache peek does not promote", "[lru][peek]") {
  data::LRUCache<std::string, int> cache(2);

  cache.put("a", 1);
  cache.put("b", 2);

  // Peek at "a" - should NOT promote it
  REQUIRE(cache.peek("a") == 1);

  // "a" is still LRU, should be evicted
  cache.put("c", 3);

  REQUIRE(cache.peek("a") == std::nullopt); // evicted despite peek
  REQUIRE(cache.peek("b") == 2);
  REQUIRE(cache.peek("c") == 3);
}

TEST_CASE("LRUCache get_ptr", "[lru][get_ptr]") {
  data::LRUCache<std::string, std::string> cache(3);

  cache.put("a", "hello");

  auto* ptr = cache.get_ptr("a");
  REQUIRE(ptr != nullptr);
  REQUIRE(*ptr == "hello");

  // Modify through pointer
  *ptr = "world";
  REQUIRE(cache.get("a") == "world");

  REQUIRE(cache.get_ptr("nonexistent") == nullptr);
}

TEST_CASE("LRUCache get_or_put", "[lru][get_or_put]") {
  data::LRUCache<std::string, int> cache(3);

  // Insert new entry
  auto& val1 = cache.get_or_put("a", 1);
  REQUIRE(val1 == 1);
  REQUIRE(cache.size() == 1);

  // Get existing entry (doesn't overwrite)
  auto& val2 = cache.get_or_put("a", 999);
  REQUIRE(val2 == 1); // Original value
  REQUIRE(cache.size() == 1);
}

TEST_CASE("LRUCache try_emplace", "[lru][try_emplace]") {
  data::LRUCache<std::string, std::string> cache(3);

  auto [val1, inserted1] = cache.try_emplace("a", "hello");
  REQUIRE(inserted1);
  REQUIRE(val1 == "hello");

  auto [val2, inserted2] = cache.try_emplace("a", "world");
  REQUIRE_FALSE(inserted2);
  REQUIRE(val2 == "hello"); // Original value
}

// ─────────────────────────────────────────────────────────────────────────────
// Iterator tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LRUCache iteration order (MRU to LRU)", "[lru][iterator]") {
  data::LRUCache<std::string, int> cache(5);

  cache.put("a", 1);
  cache.put("b", 2);
  cache.put("c", 3);

  // Access "a" to make it MRU
  (void)cache.get("a");

  // Order should be: a (MRU), c, b (LRU)
  std::vector<std::string> keys;
  for (auto [key, value] : cache) {
    keys.push_back(std::string(key));
  }

  REQUIRE(keys.size() == 3);
  REQUIRE(keys[0] == "a"); // MRU
  REQUIRE(keys[1] == "c");
  REQUIRE(keys[2] == "b"); // LRU
}

TEST_CASE("LRUCache front and back", "[lru][endpoints]") {
  data::LRUCache<std::string, int> cache(3);

  REQUIRE(cache.front() == std::nullopt);
  REQUIRE(cache.back() == std::nullopt);

  cache.put("a", 1);
  cache.put("b", 2);
  cache.put("c", 3);

  // Front should be MRU ("c"), back should be LRU ("a")
  auto front = cache.front();
  auto back = cache.back();

  REQUIRE(front.has_value());
  REQUIRE(front->first == "c");
  REQUIRE(front->second == 3);

  REQUIRE(back.has_value());
  REQUIRE(back->first == "a");
  REQUIRE(back->second == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Eviction callback tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LRUCache eviction callback", "[lru][callback]") {
  std::vector<std::pair<std::string, int>> evicted;

  data::LRUCache<std::string, int> cache(
      2, [&](const std::string& key, int& value) { evicted.emplace_back(key, value); });

  cache.put("a", 1);
  cache.put("b", 2);
  cache.put("c", 3); // Should evict "a"

  REQUIRE(evicted.size() == 1);
  REQUIRE(evicted[0].first == "a");
  REQUIRE(evicted[0].second == 1);
}

TEST_CASE("LRUCache eviction callback on clear", "[lru][callback]") {
  std::vector<std::string> evicted;

  data::LRUCache<std::string, int> cache(
      3, [&](const std::string& key, int&) { evicted.push_back(key); });

  cache.put("a", 1);
  cache.put("b", 2);
  cache.put("c", 3);

  cache.clear();

  REQUIRE(evicted.size() == 3);
}

TEST_CASE("LRUCache set_eviction_callback", "[lru][callback]") {
  std::vector<int> evicted;

  data::LRUCache<int, int> cache(2);

  cache.put(1, 100);
  cache.put(2, 200);

  // Set callback after creation
  cache.set_eviction_callback([&](const int& key, int&) { evicted.push_back(key); });

  cache.put(3, 300); // Should evict 1

  REQUIRE(evicted.size() == 1);
  REQUIRE(evicted[0] == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Resize tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LRUCache resize larger", "[lru][resize]") {
  data::LRUCache<int, int> cache(2);

  cache.put(1, 100);
  cache.put(2, 200);

  cache.resize(5);

  REQUIRE(cache.capacity() == 5);
  REQUIRE(cache.size() == 2);

  cache.put(3, 300);
  cache.put(4, 400);
  cache.put(5, 500);

  REQUIRE(cache.size() == 5);
  REQUIRE(cache.get(1) == 100); // Still present
}

TEST_CASE("LRUCache resize smaller triggers eviction", "[lru][resize]") {
  std::vector<int> evicted;

  data::LRUCache<int, int> cache(5, [&](const int& key, int&) { evicted.push_back(key); });

  cache.put(1, 100);
  cache.put(2, 200);
  cache.put(3, 300);
  cache.put(4, 400);
  cache.put(5, 500);

  cache.resize(2);

  REQUIRE(cache.capacity() == 2);
  REQUIRE(cache.size() == 2);
  REQUIRE(evicted.size() == 3);

  // LRU entries (1, 2, 3) should be evicted
  REQUIRE(cache.get(1) == std::nullopt);
  REQUIRE(cache.get(2) == std::nullopt);
  REQUIRE(cache.get(3) == std::nullopt);
  REQUIRE(cache.get(4) == 400);
  REQUIRE(cache.get(5) == 500);
}

// ─────────────────────────────────────────────────────────────────────────────
// Zero capacity edge case
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LRUCache zero capacity", "[lru][edge]") {
  data::LRUCache<std::string, int> cache(0);

  REQUIRE(cache.capacity() == 0);
  REQUIRE(cache.size() == 0);

  // Put should fail silently
  REQUIRE_FALSE(cache.put("a", 1));
  REQUIRE(cache.size() == 0);
  REQUIRE(cache.get("a") == std::nullopt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Move semantics tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LRUCache with move-only values", "[lru][move]") {
  data::LRUCache<std::string, std::unique_ptr<int>> cache(3);

  cache.put("a", std::make_unique<int>(42));

  auto* ptr = cache.get_ptr("a");
  REQUIRE(ptr != nullptr);
  REQUIRE(**ptr == 42);
}

TEST_CASE("LRUCache move construction", "[lru][move]") {
  data::LRUCache<std::string, int> cache1(3);
  cache1.put("a", 1);
  cache1.put("b", 2);

  data::LRUCache<std::string, int> cache2(std::move(cache1));

  REQUIRE(cache2.size() == 2);
  REQUIRE(cache2.get("a") == 1);
  REQUIRE(cache2.get("b") == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// LRUCacheSafe tests (thread-safe variant)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LRUCacheSafe basic operations", "[lru][threadsafe]") {
  data::LRUCacheSafe<std::string, int> cache(3);

  cache.put("a", 1);
  cache.put("b", 2);

  REQUIRE(cache.get("a") == 1);
  REQUIRE(cache.get("b") == 2);
  REQUIRE(cache.get("c") == std::nullopt);

  REQUIRE(cache.size() == 2);
  REQUIRE(cache.contains("a"));
  REQUIRE_FALSE(cache.contains("c"));
}

TEST_CASE("LRUCacheSafe concurrent access", "[lru][threadsafe]") {
  data::LRUCacheSafe<int, int> cache(1000);

  constexpr int num_threads = 4;
  constexpr int ops_per_thread = 1000;

  std::vector<std::thread> threads;
  std::atomic<int> total_puts{0};

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < ops_per_thread; ++i) {
        int key = t * ops_per_thread + i;
        cache.put(key, key * 10);
        total_puts.fetch_add(1);

        // Interleave reads
        (void)cache.get(key);
        (void)cache.contains(key);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  REQUIRE(total_puts.load() == num_threads * ops_per_thread);
  REQUIRE(cache.size() <= cache.capacity());
}

TEST_CASE("LRUCacheSafe peek vs get", "[lru][threadsafe]") {
  data::LRUCacheSafe<std::string, int> cache(3);

  cache.put("a", 1);
  cache.put("b", 2);

  // peek uses shared lock, get uses unique lock
  REQUIRE(cache.peek("a") == 1);
  REQUIRE(cache.get("a") == 1);
}

TEST_CASE("LRUCacheSafe with_lock for batch operations", "[lru][threadsafe]") {
  data::LRUCacheSafe<std::string, int> cache(10);

  cache.with_lock([](auto& c) {
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3);
  });

  REQUIRE(cache.size() == 3);

  auto sum = cache.with_shared_lock([](const auto& c) {
    int s = 0;
    for (auto [key, value] : c) {
      s += value;
    }
    return s;
  });

  REQUIRE(sum == 6);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LRUCache size never exceeds capacity", "[lru][property]") {
  rc::prop("size <= capacity always holds", []() {
    auto capacity = *rc::gen::inRange<std::size_t>(1, 100);
    data::LRUCache<int, int> cache(capacity);

    auto ops = *rc::gen::inRange<int>(0, 200);
    for (int i = 0; i < ops; ++i) {
      cache.put(i, i * 10);
    }

    RC_ASSERT(cache.size() <= cache.capacity());
  });
}

TEST_CASE("LRUCache get after put returns correct value", "[lru][property]") {
  rc::prop("put then get returns same value", []() {
    data::LRUCache<int, int> cache(100);

    auto key = *rc::gen::arbitrary<int>();
    auto value = *rc::gen::arbitrary<int>();

    cache.put(key, value);
    auto result = cache.get(key);

    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == value);
  });
}

TEST_CASE("LRUCache erase makes key unavailable", "[lru][property]") {
  rc::prop("erase removes key", []() {
    data::LRUCache<int, int> cache(100);

    auto key = *rc::gen::arbitrary<int>();
    auto value = *rc::gen::arbitrary<int>();

    cache.put(key, value);
    RC_ASSERT(cache.contains(key));

    cache.erase(key);
    RC_ASSERT(!cache.contains(key));
    RC_ASSERT(cache.get(key) == std::nullopt);
  });
}

TEST_CASE("LRUCache update preserves key", "[lru][property]") {
  rc::prop("update doesn't change size", []() {
    data::LRUCache<int, int> cache(100);

    auto key = *rc::gen::arbitrary<int>();
    auto value1 = *rc::gen::arbitrary<int>();
    auto value2 = *rc::gen::arbitrary<int>();

    cache.put(key, value1);
    auto size_after_first = cache.size();

    cache.put(key, value2);
    auto size_after_second = cache.size();

    RC_ASSERT(size_after_first == size_after_second);
    RC_ASSERT(cache.get(key) == value2);
  });
}

TEST_CASE("LRUCache LRU eviction order", "[lru][property]") {
  rc::prop("oldest untouched entry is evicted", []() {
    constexpr std::size_t capacity = 5;
    data::LRUCache<int, int> cache(capacity);

    // Fill cache
    for (std::size_t i = 0; i < capacity; ++i) {
      cache.put(static_cast<int>(i), static_cast<int>(i * 10));
    }

    // Access all except key 0 (making 0 the LRU)
    for (std::size_t i = 1; i < capacity; ++i) {
      (void)cache.get(static_cast<int>(i));
    }

    // Insert new key, should evict 0
    cache.put(100, 1000);

    RC_ASSERT(cache.get(0) == std::nullopt); // evicted
    RC_ASSERT(cache.get(100) == 1000);       // present
  });
}

TEST_CASE("LRUCache iteration visits all entries", "[lru][property]") {
  rc::prop("iteration count equals size", []() {
    auto capacity = *rc::gen::inRange<std::size_t>(1, 50);
    data::LRUCache<int, int> cache(capacity);

    auto num_entries = *rc::gen::inRange<std::size_t>(0, capacity);
    for (std::size_t i = 0; i < num_entries; ++i) {
      cache.put(static_cast<int>(i), static_cast<int>(i * 10));
    }

    std::size_t count = 0;
    for (auto [key, value] : cache) {
      (void)key;
      (void)value;
      ++count;
    }

    RC_ASSERT(count == cache.size());
  });
}

TEST_CASE("LRUCache clear empties cache", "[lru][property]") {
  rc::prop("clear results in empty cache", []() {
    auto capacity = *rc::gen::inRange<std::size_t>(1, 50);
    data::LRUCache<int, int> cache(capacity);

    auto num_entries = *rc::gen::inRange<std::size_t>(0, capacity);
    for (std::size_t i = 0; i < num_entries; ++i) {
      cache.put(static_cast<int>(i), static_cast<int>(i * 10));
    }

    cache.clear();

    RC_ASSERT(cache.size() == 0);
    RC_ASSERT(cache.empty());
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Heterogeneous lookup tests
// ─────────────────────────────────────────────────────────────────────────────

// Note: Heterogeneous lookup with std::unordered_map requires transparent
// hash and comparator (C++20). The default std::hash<std::string> doesn't
// support transparent lookup. For string keys with string_view lookup,
// users need to provide a custom transparent hasher.
//
// Example usage for heterogeneous lookup:
//   struct StringHash {
//     using is_transparent = void;
//     std::size_t operator()(std::string_view sv) const { ... }
//   };
//   struct StringEqual {
//     using is_transparent = void;
//     bool operator()(std::string_view a, std::string_view b) const { ... }
//   };
//   data::LRUCache<std::string, int, StringHash, StringEqual> cache(100);
