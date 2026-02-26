// straylight // nix // util // tests
//
// Unit tests for LRU cache

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/lru-cache.h"


// ─────────────────────────────────────────────────────────────────────────────
// Basic operations
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("lru cache get on empty cache returns nullopt", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(10);
  REQUIRE(cache.get(42) == std::nullopt);
  REQUIRE(cache.size() == 0);
}

TEST_CASE("lru cache upsert and get basic operation", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(10);

  cache.upsert(1, "one");
  cache.upsert(2, "two");
  cache.upsert(3, "three");

  REQUIRE(cache.size() == 3);
  REQUIRE(cache.get(1) == "one");
  REQUIRE(cache.get(2) == "two");
  REQUIRE(cache.get(3) == "three");
}

TEST_CASE("lru cache upsert overwrites existing value", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(10);

  cache.upsert(1, "original");
  REQUIRE(cache.get(1) == "original");

  cache.upsert(1, "updated");
  REQUIRE(cache.get(1) == "updated");
  REQUIRE(cache.size() == 1);
}

TEST_CASE("lru cache getOrNullptr returns pointer or nullptr", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(10);

  REQUIRE(cache.get_or_nullptr(1) == nullptr);

  cache.upsert(1, "one");
  auto* value_pointer = cache.get_or_nullptr(1);
  REQUIRE(value_pointer != nullptr);
  REQUIRE(*value_pointer == "one");

  // verify mutation through pointer
  *value_pointer = "modified";
  REQUIRE(cache.get(1) == "modified");
}

TEST_CASE("lru cache erase removes element", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(10);

  cache.upsert(1, "one");
  cache.upsert(2, "two");
  REQUIRE(cache.size() == 2);

  REQUIRE(cache.erase(1) == true);
  REQUIRE(cache.size() == 1);
  REQUIRE(cache.get(1) == std::nullopt);
  REQUIRE(cache.get(2) == "two");

  // erasing non-existent key returns false
  REQUIRE(cache.erase(999) == false);
}

TEST_CASE("lru cache clear removes all elements", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(10);

  cache.upsert(1, "one");
  cache.upsert(2, "two");
  cache.upsert(3, "three");
  REQUIRE(cache.size() == 3);

  cache.clear();
  REQUIRE(cache.size() == 0);
  REQUIRE(cache.get(1) == std::nullopt);
  REQUIRE(cache.get(2) == std::nullopt);
  REQUIRE(cache.get(3) == std::nullopt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Eviction behavior
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("lru cache evicts oldest when capacity exceeded", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(3);

  cache.upsert(1, "one");
  cache.upsert(2, "two");
  cache.upsert(3, "three");
  REQUIRE(cache.size() == 3);

  // inserting fourth element should evict the oldest (1)
  cache.upsert(4, "four");
  REQUIRE(cache.size() == 3);
  REQUIRE(cache.get(1) == std::nullopt);
  REQUIRE(cache.get(2) == "two");
  REQUIRE(cache.get(3) == "three");
  REQUIRE(cache.get(4) == "four");
}

TEST_CASE("lru cache get promotes item to most recently used", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(3);

  cache.upsert(1, "one");
  cache.upsert(2, "two");
  cache.upsert(3, "three");

  // access element 1, making it most recently used
  REQUIRE(cache.get(1) == "one");

  // now insert element 4, which should evict element 2 (now oldest)
  cache.upsert(4, "four");
  REQUIRE(cache.get(1) == "one");
  REQUIRE(cache.get(2) == std::nullopt);
  REQUIRE(cache.get(3) == "three");
  REQUIRE(cache.get(4) == "four");
}

TEST_CASE("lru cache getOrNullptr promotes item to most recently used", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(3);

  cache.upsert(1, "one");
  cache.upsert(2, "two");
  cache.upsert(3, "three");

  // access element 1 via getOrNullptr, making it most recently used
  REQUIRE(cache.get_or_nullptr(1) != nullptr);

  // now insert element 4, which should evict element 2 (now oldest)
  cache.upsert(4, "four");
  REQUIRE(cache.get(1) == "one");
  REQUIRE(cache.get(2) == std::nullopt);
}

TEST_CASE("lru cache upsert existing key promotes to most recently used", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(3);

  cache.upsert(1, "one");
  cache.upsert(2, "two");
  cache.upsert(3, "three");

  // update element 1, making it most recently used
  cache.upsert(1, "one_updated");

  // insert element 4, which should evict element 2 (now oldest)
  cache.upsert(4, "four");
  REQUIRE(cache.get(1) == "one_updated");
  REQUIRE(cache.get(2) == std::nullopt);
  REQUIRE(cache.get(3) == "three");
  REQUIRE(cache.get(4) == "four");
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("lru cache with zero capacity ignores all inserts", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(0);

  cache.upsert(1, "one");
  cache.upsert(2, "two");

  REQUIRE(cache.size() == 0);
  REQUIRE(cache.get(1) == std::nullopt);
  REQUIRE(cache.get(2) == std::nullopt);
}

TEST_CASE("lru cache with capacity one evicts on every new insert", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(1);

  cache.upsert(1, "one");
  REQUIRE(cache.size() == 1);
  REQUIRE(cache.get(1) == "one");

  cache.upsert(2, "two");
  REQUIRE(cache.size() == 1);
  REQUIRE(cache.get(1) == std::nullopt);
  REQUIRE(cache.get(2) == "two");

  cache.upsert(3, "three");
  REQUIRE(cache.size() == 1);
  REQUIRE(cache.get(2) == std::nullopt);
  REQUIRE(cache.get(3) == "three");
}

TEST_CASE("lru cache thrashing with capacity smaller than working set", "[lru-cache]") {
  nix::lru_cache_t<int, int> cache(3);

  // simulate thrashing: continuously access more keys than capacity
  for (int round = 0; round < 5; ++round) {
    for (int key = 0; key < 10; ++key) {
      cache.upsert(key, key * 100);
    }
  }

  // only the last 3 keys should remain
  REQUIRE(cache.size() == 3);
  REQUIRE(cache.get(7) == 700);
  REQUIRE(cache.get(8) == 800);
  REQUIRE(cache.get(9) == 900);

  // earlier keys should be evicted
  REQUIRE(cache.get(0) == std::nullopt);
  REQUIRE(cache.get(6) == std::nullopt);
}

TEST_CASE("lru cache complex eviction sequence", "[lru-cache]") {
  nix::lru_cache_t<int, int> cache(4);

  // fill cache: order is 1, 2, 3, 4 (1 is oldest)
  cache.upsert(1, 100);
  cache.upsert(2, 200);
  cache.upsert(3, 300);
  cache.upsert(4, 400);

  // access 1 and 2, making order: 3, 4, 1, 2
  cache.get(1);
  cache.get(2);

  // insert 5, should evict 3 (oldest)
  cache.upsert(5, 500);
  REQUIRE(cache.get(3) == std::nullopt);
  REQUIRE(cache.get(4) == 400);

  // order now: 1, 2, 4, 5 (after accessing 4)
  // insert 6, should evict 1
  cache.upsert(6, 600);
  REQUIRE(cache.get(1) == std::nullopt);

  // verify remaining elements
  REQUIRE(cache.get(2) == 200);
  REQUIRE(cache.get(4) == 400);
  REQUIRE(cache.get(5) == 500);
  REQUIRE(cache.get(6) == 600);
}

TEST_CASE("lru cache with string keys", "[lru-cache]") {
  nix::lru_cache_t<std::string, int> cache(3);

  cache.upsert("alpha", 1);
  cache.upsert("beta", 2);
  cache.upsert("gamma", 3);

  REQUIRE(cache.get("alpha") == 1);
  REQUIRE(cache.get("beta") == 2);
  REQUIRE(cache.get("gamma") == 3);
  REQUIRE(cache.get("delta") == std::nullopt);

  // verify heterogeneous lookup with string_view
  std::string_view key_view = "alpha";
  REQUIRE(cache.get(key_view) == 1);
}

TEST_CASE("lru cache erase then reinsert same key", "[lru-cache]") {
  nix::lru_cache_t<int, std::string> cache(3);

  cache.upsert(1, "one");
  cache.upsert(2, "two");
  cache.upsert(3, "three");

  cache.erase(2);
  REQUIRE(cache.size() == 2);
  REQUIRE(cache.get(2) == std::nullopt);

  cache.upsert(2, "two_again");
  REQUIRE(cache.size() == 3);
  REQUIRE(cache.get(2) == "two_again");

  // insert another element, should evict element 1 (oldest remaining)
  cache.upsert(4, "four");
  REQUIRE(cache.get(1) == std::nullopt);
  REQUIRE(cache.get(2) == "two_again");
  REQUIRE(cache.get(3) == "three");
  REQUIRE(cache.get(4) == "four");
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests with RapidCheck
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("lru cache property tests", "[lru-cache][property]") {
  rc::prop("size never exceeds capacity", []() {
    auto capacity = *rc::gen::inRange<size_t>(1, 100);
    auto operations = *rc::gen::inRange<size_t>(0, 500);

    nix::lru_cache_t<int, int> cache(capacity);

    for (size_t i = 0; i < operations; ++i) {
      auto key = *rc::gen::inRange<int>(0, 200);
      auto value = *rc::gen::arbitrary<int>();
      cache.upsert(key, value);
      RC_ASSERT(cache.size() <= capacity);
    }
  });

  rc::prop("get returns value that was inserted", []() {
    auto capacity = *rc::gen::inRange<size_t>(1, 50);
    nix::lru_cache_t<int, int> cache(capacity);

    auto key = *rc::gen::arbitrary<int>();
    auto value = *rc::gen::arbitrary<int>();

    cache.upsert(key, value);

    // immediately after insert, get should return the value
    auto result = cache.get(key);
    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == value);
  });

  rc::prop("upsert overwrites previous value", []() {
    auto capacity = *rc::gen::inRange<size_t>(1, 50);
    nix::lru_cache_t<int, int> cache(capacity);

    auto key = *rc::gen::arbitrary<int>();
    auto value_first = *rc::gen::arbitrary<int>();
    auto value_second = *rc::gen::arbitrary<int>();

    cache.upsert(key, value_first);
    cache.upsert(key, value_second);

    auto result = cache.get(key);
    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == value_second);
    RC_ASSERT(cache.size() == 1);
  });

  rc::prop("erase removes element and get returns nullopt", []() {
    auto capacity = *rc::gen::inRange<size_t>(1, 50);
    nix::lru_cache_t<int, int> cache(capacity);

    auto key = *rc::gen::arbitrary<int>();
    auto value = *rc::gen::arbitrary<int>();

    cache.upsert(key, value);
    RC_ASSERT(cache.get(key).has_value());

    cache.erase(key);
    RC_ASSERT(!cache.get(key).has_value());
  });

  rc::prop("clear empties the cache", []() {
    auto capacity = *rc::gen::inRange<size_t>(1, 50);
    auto num_elements = *rc::gen::inRange<size_t>(0, 100);

    nix::lru_cache_t<int, int> cache(capacity);

    std::vector<int> inserted_keys;
    for (size_t i = 0; i < num_elements; ++i) {
      auto key = *rc::gen::arbitrary<int>();
      auto value = *rc::gen::arbitrary<int>();
      cache.upsert(key, value);
      inserted_keys.push_back(key);
    }

    cache.clear();
    RC_ASSERT(cache.size() == 0);

    for (int key : inserted_keys) {
      RC_ASSERT(!cache.get(key).has_value());
    }
  });

  rc::prop("recently accessed elements survive eviction", []() {
    auto capacity = *rc::gen::inRange<size_t>(2, 20);
    nix::lru_cache_t<int, int> cache(capacity);

    // fill cache to capacity with keys 0 to capacity-1
    for (size_t i = 0; i < capacity; ++i) {
      cache.upsert(static_cast<int>(i), static_cast<int>(i * 100));
    }

    // access the first element to make it most recently used
    cache.get(0);

    // insert capacity more elements to force eviction of all except element 0
    for (size_t i = capacity; i < (2 * capacity) - 1; ++i) {
      cache.upsert(static_cast<int>(i), static_cast<int>(i * 100));
    }

    // element 0 should still be present (it was accessed, making it recent)
    RC_ASSERT(cache.get(0).has_value());
  });
}

TEST_CASE("lru cache eviction order property", "[lru-cache][property]") {
  rc::prop("oldest untouched element is evicted first", []() {
    auto capacity = *rc::gen::inRange<size_t>(3, 20);
    nix::lru_cache_t<int, int> cache(capacity);

    // insert elements 0 through capacity-1
    for (size_t i = 0; i < capacity; ++i) {
      cache.upsert(static_cast<int>(i), static_cast<int>(i));
    }

    // access all elements except element 0
    for (size_t i = 1; i < capacity; ++i) {
      cache.get(static_cast<int>(i));
    }

    // insert a new element, which should evict element 0
    cache.upsert(static_cast<int>(capacity), static_cast<int>(capacity));

    RC_ASSERT(!cache.get(0).has_value());
    RC_ASSERT(cache.get(static_cast<int>(capacity)).has_value());
  });
}

TEST_CASE("lru cache stress test with mixed operations", "[lru-cache][property]") {
  rc::prop("cache remains consistent after mixed operations", []() {
    auto capacity = *rc::gen::inRange<size_t>(1, 30);
    auto num_operations = *rc::gen::inRange<size_t>(10, 200);

    nix::lru_cache_t<int, int> cache(capacity);

    for (size_t op = 0; op < num_operations; ++op) {
      auto operation_type = *rc::gen::inRange<int>(0, 4);
      auto key = *rc::gen::inRange<int>(0, 50);
      auto value = *rc::gen::arbitrary<int>();

      switch (operation_type) {
        case 0: // upsert
          cache.upsert(key, value);
          break;
        case 1: // get
          cache.get(key);
          break;
        case 2: // getOrNullptr
          cache.get_or_nullptr(key);
          break;
        case 3: // erase
          cache.erase(key);
          break;
        default:
          break;
      }

      // invariant: size never exceeds capacity
      RC_ASSERT(cache.size() <= capacity);
    }

    // after all operations, verify cache is in consistent state
    // (no crashes, size within bounds)
    RC_ASSERT(cache.size() <= capacity);
  });
}
