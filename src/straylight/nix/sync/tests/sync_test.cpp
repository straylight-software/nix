// straylight::nix::sync::sync tests
//
// Property-based testing with rapidcheck for synchronized value primitives.
// Tests thread-safe access, locking semantics, and condition variables.

// IMPORTANT: Catch2 v3 MUST be included BEFORE rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <map>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "../sync.h"

namespace prims = straylight::nix::sync;

// ─────────────────────────────────────────────────────────────────────────────
// Basic construction tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Sync default construction", "[sync][construction]") {
  prims::Sync<int> s;
  auto lock = s.lock();
  REQUIRE(*lock == 0);
}

TEST_CASE("Sync copy construction", "[sync][construction]") {
  prims::Sync<int> s(42);
  auto lock = s.lock();
  REQUIRE(*lock == 42);
}

TEST_CASE("Sync move construction", "[sync][construction]") {
  std::string str = "hello world";
  prims::Sync<std::string> s(std::move(str));
  auto lock = s.lock();
  REQUIRE(*lock == "hello world");
}

TEST_CASE("Sync in-place construction", "[sync][construction]") {
  prims::Sync<std::vector<int>> s(std::in_place, 5, 42);
  auto lock = s.lock();
  REQUIRE(lock->size() == 5);
  REQUIRE((*lock)[0] == 42);
}

TEST_CASE("Sync construction property", "[sync][construction][property]") {
  rc::prop("Sync<int> preserves initial value", []() {
    int value = *rc::gen::arbitrary<int>();
    prims::Sync<int> s(value);
    auto lock = s.lock();
    RC_ASSERT(*lock == value);
  });
}

TEST_CASE("Sync string construction property", "[sync][construction][property]") {
  rc::prop("Sync<string> preserves initial value", []() {
    std::string value = *rc::gen::arbitrary<std::string>();
    prims::Sync<std::string> s(value);
    auto lock = s.lock();
    RC_ASSERT(*lock == value);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Lock proxy tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("WriteLock operator-> and operator*", "[sync][lock]") {
  struct Data {
    int x = 10;
    int y = 20;
  };

  prims::Sync<Data> s;
  {
    auto lock = s.lock();
    REQUIRE(lock->x == 10);
    REQUIRE(lock->y == 20);
    REQUIRE((*lock).x == 10);

    lock->x = 100;
    REQUIRE(lock->x == 100);
  }

  // Verify change persisted
  auto lock = s.lock();
  REQUIRE(lock->x == 100);
}

TEST_CASE("WriteLock get() returns raw pointer", "[sync][lock]") {
  prims::Sync<int> s(42);
  auto lock = s.lock();
  int* ptr = lock.get();
  REQUIRE(ptr != nullptr);
  REQUIRE(*ptr == 42);
  *ptr = 100;
  REQUIRE(*lock == 100);
}

TEST_CASE("WriteLock owns_lock()", "[sync][lock]") {
  prims::Sync<int> s(42);
  auto lock = s.lock();
  REQUIRE(lock.owns_lock());

  lock.unlock();
  REQUIRE_FALSE(lock.owns_lock());
  REQUIRE(lock.get() == nullptr);
}

TEST_CASE("ReadLock provides const access", "[sync][lock]") {
  prims::Sync<int> s(42);
  auto lock = s.read_lock();
  REQUIRE(*lock == 42);
  REQUIRE(lock.get() != nullptr);
  REQUIRE(lock.owns_lock());

  // Compile-time check: the following should not compile
  // *lock = 100; // ERROR: assignment of read-only location
}

// ─────────────────────────────────────────────────────────────────────────────
// try_lock tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("try_lock succeeds when unlocked", "[sync][try_lock]") {
  prims::Sync<int> s(42);
  auto maybe_lock = s.try_lock();
  REQUIRE(maybe_lock.has_value());
  REQUIRE(**maybe_lock == 42);
}

TEST_CASE("try_lock fails when already locked", "[sync][try_lock]") {
  prims::Sync<int> s(42);
  auto lock1 = s.lock();

  std::thread t([&] {
    auto maybe_lock = s.try_lock();
    REQUIRE_FALSE(maybe_lock.has_value());
  });
  t.join();
}

TEST_CASE("try_read_lock succeeds when unlocked", "[sync][try_lock]") {
  prims::Sync<int> s(42);
  auto maybe_lock = s.try_read_lock();
  REQUIRE(maybe_lock.has_value());
  REQUIRE(**maybe_lock == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// with_lock tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("with_lock executes callable", "[sync][with_lock]") {
  prims::Sync<int> s(42);
  s.with_lock([](int& value) { value = 100; });

  auto lock = s.lock();
  REQUIRE(*lock == 100);
}

TEST_CASE("with_lock returns result", "[sync][with_lock]") {
  prims::Sync<int> s(42);
  int result = s.with_lock([](int& value) { return value * 2; });
  REQUIRE(result == 84);
}

TEST_CASE("with_lock void callable", "[sync][with_lock]") {
  prims::Sync<std::vector<int>> s;
  s.with_lock([](std::vector<int>& vec) {
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
  });

  auto lock = s.lock();
  REQUIRE(lock->size() == 3);
  REQUIRE((*lock)[0] == 1);
  REQUIRE((*lock)[2] == 3);
}

TEST_CASE("with_read_lock provides const access", "[sync][with_lock]") {
  prims::Sync<int> s(42);
  int result = s.with_read_lock([](const int& value) { return value + 1; });
  REQUIRE(result == 43);

  // Original unchanged
  auto lock = s.lock();
  REQUIRE(*lock == 42);
}

TEST_CASE("try_with_lock succeeds when unlocked", "[sync][with_lock]") {
  prims::Sync<int> s(42);
  auto result = s.try_with_lock([](int& value) {
    value = 100;
    return value;
  });
  REQUIRE(result.has_value());
  REQUIRE(*result == 100);
}

TEST_CASE("try_with_lock fails when locked", "[sync][with_lock]") {
  prims::Sync<int> s(42);
  auto lock = s.lock();

  std::thread t([&] {
    auto result = s.try_with_lock([](int& value) { return value * 2; });
    REQUIRE_FALSE(result.has_value());
  });
  t.join();
}

TEST_CASE("with_lock property", "[sync][with_lock][property]") {
  rc::prop("with_lock sees same value as lock()", []() {
    int initial = *rc::gen::arbitrary<int>();
    prims::Sync<int> s(initial);

    int via_lock = [&] {
      auto lock = s.lock();
      return *lock;
    }();

    int via_with = s.with_lock([](int& v) { return v; });

    RC_ASSERT(via_lock == via_with);
    RC_ASSERT(via_lock == initial);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// SharedSync tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SharedSync allows concurrent readers", "[sync][shared]") {
  prims::SharedSync<int> s(42);

  std::atomic<int> concurrent_readers{0};
  std::atomic<int> max_concurrent{0};
  std::latch start_latch(4);

  auto reader = [&] {
    start_latch.arrive_and_wait();
    auto lock = s.read_lock();
    int current = concurrent_readers.fetch_add(1) + 1;
    int expected = max_concurrent.load();
    while (current > expected && !max_concurrent.compare_exchange_weak(expected, current)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    concurrent_readers.fetch_sub(1);
    REQUIRE(*lock == 42);
  };

  std::thread t1(reader);
  std::thread t2(reader);
  std::thread t3(reader);
  std::thread t4(reader);

  t1.join();
  t2.join();
  t3.join();
  t4.join();

  // Should have had concurrent readers
  REQUIRE(max_concurrent.load() > 1);
}

TEST_CASE("SharedSync writer blocks readers", "[sync][shared]") {
  prims::SharedSync<int> s(42);
  std::atomic<bool> writer_done{false};
  std::atomic<bool> reader_saw_update{false};

  std::thread writer([&] {
    auto lock = s.lock();
    *lock = 100;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    writer_done.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  std::thread reader([&] {
    auto lock = s.read_lock();
    // Should only run after writer releases
    reader_saw_update.store(*lock == 100);
  });

  writer.join();
  reader.join();

  REQUIRE(writer_done.load());
  REQUIRE(reader_saw_update.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// Copy/Move semantics tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Sync copy constructor", "[sync][copy]") {
  prims::Sync<int> s1(42);
  prims::Sync<int> s2(s1);

  auto lock1 = s1.lock();
  auto lock2 = s2.lock();

  REQUIRE(*lock1 == 42);
  REQUIRE(*lock2 == 42);

  // Modifying one doesn't affect the other
  *lock1 = 100;
  REQUIRE(*lock1 == 100);
  REQUIRE(*lock2 == 42);
}

TEST_CASE("Sync move constructor", "[sync][move]") {
  prims::Sync<std::vector<int>> s1(std::in_place, std::initializer_list<int>{1, 2, 3});
  prims::Sync<std::vector<int>> s2(std::move(s1));

  auto lock = s2.lock();
  REQUIRE(lock->size() == 3);
  REQUIRE((*lock)[0] == 1);
}

TEST_CASE("Sync copy assignment", "[sync][copy]") {
  prims::Sync<int> s1(42);
  prims::Sync<int> s2(100);

  s2 = s1;

  auto lock = s2.lock();
  REQUIRE(*lock == 42);
}

TEST_CASE("Sync copy()", "[sync][copy]") {
  prims::Sync<std::string> s("hello");
  std::string copy = s.copy();
  REQUIRE(copy == "hello");

  // Modify original
  s.with_lock([](std::string& str) { str = "world"; });

  // Copy is independent
  REQUIRE(copy == "hello");

  // Original changed
  REQUIRE(s.copy() == "world");
}

TEST_CASE("Sync swap with value", "[sync][swap]") {
  prims::Sync<int> s(42);
  int value = 100;

  s.swap(value);

  REQUIRE(value == 42);
  auto lock = s.lock();
  REQUIRE(*lock == 100);
}

TEST_CASE("Sync swap with Sync", "[sync][swap]") {
  prims::Sync<int> s1(42);
  prims::Sync<int> s2(100);

  s1.swap(s2);

  REQUIRE(*s1.lock() == 100);
  REQUIRE(*s2.lock() == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrent access tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Concurrent increments", "[sync][concurrent]") {
  prims::Sync<int> counter(0);
  constexpr int num_threads = 8;
  constexpr int increments_per_thread = 1000;

  std::vector<std::thread> threads;
  threads.reserve(num_threads);

  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < increments_per_thread; ++j) {
        auto lock = counter.lock();
        ++(*lock);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  auto lock = counter.lock();
  REQUIRE(*lock == num_threads * increments_per_thread);
}

TEST_CASE("Concurrent map operations", "[sync][concurrent]") {
  prims::Sync<std::map<int, int>> map;
  constexpr int num_threads = 4;
  constexpr int ops_per_thread = 100;

  std::vector<std::thread> threads;
  threads.reserve(num_threads);

  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&, i] {
      for (int j = 0; j < ops_per_thread; ++j) {
        int key = i * ops_per_thread + j;
        map.with_lock([&](std::map<int, int>& m) { m[key] = key * 2; });
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  auto lock = map.lock();
  REQUIRE(lock->size() == static_cast<std::size_t>(num_threads * ops_per_thread));

  for (const auto& [key, value] : *lock) {
    REQUIRE(value == key * 2);
  }
}

TEST_CASE("Concurrent increment property", "[sync][concurrent][property]") {
  rc::prop("concurrent increments are serialized", []() {
    int num_threads = *rc::gen::inRange(2, 8);
    int increments = *rc::gen::inRange(10, 100);

    prims::Sync<int> counter(0);
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(num_threads));

    for (int i = 0; i < num_threads; ++i) {
      threads.emplace_back([&, increments] {
        for (int j = 0; j < increments; ++j) {
          counter.with_lock([](int& c) { ++c; });
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    RC_ASSERT(*counter.lock() == num_threads * increments);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Condition variable tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("WriteLock wait on condition variable", "[sync][cv]") {
  prims::Sync<bool> ready(false);
  std::condition_variable cv;

  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
      auto lock = ready.lock();
      *lock = true;
    }
    cv.notify_one();
  });

  std::thread consumer([&] {
    auto lock = ready.lock();
    lock.wait(cv, [&] { return *lock; });
    REQUIRE(*lock == true);
  });

  producer.join();
  consumer.join();
}

TEST_CASE("WriteLock wait_for timeout", "[sync][cv]") {
  prims::Sync<bool> ready(false);
  std::condition_variable cv;

  auto lock = ready.lock();
  auto status = lock.wait_for(cv, std::chrono::milliseconds(10));
  REQUIRE(status == std::cv_status::timeout);
}

TEST_CASE("SyncWithCV basic usage", "[sync][cv]") {
  prims::SyncWithCV<int> data(0);

  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    data.with_lock_notify_one([](int& value) { value = 42; });
  });

  std::thread consumer([&] {
    auto lock = data.lock();
    data.wait(lock, [&] { return *lock != 0; });
    REQUIRE(*lock == 42);
  });

  producer.join();
  consumer.join();
}

TEST_CASE("SyncWithCV notify_all", "[sync][cv]") {
  prims::SyncWithCV<int> data(0);
  std::atomic<int> consumers_done{0};

  auto consumer = [&] {
    auto lock = data.lock();
    data.wait(lock, [&] { return *lock != 0; });
    consumers_done.fetch_add(1);
  };

  std::thread c1(consumer);
  std::thread c2(consumer);
  std::thread c3(consumer);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  data.with_lock_notify_all([](int& value) { value = 42; });

  c1.join();
  c2.join();
  c3.join();

  REQUIRE(consumers_done.load() == 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Unsafe access tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("unsafe_get provides direct access", "[sync][unsafe]") {
  prims::Sync<int> s(42);

  int& ref = s.unsafe_get();
  REQUIRE(ref == 42);

  ref = 100;
  REQUIRE(s.unsafe_get() == 100);

  // Verify via lock
  auto lock = s.lock();
  REQUIRE(*lock == 100);
}

TEST_CASE("mutex() returns underlying mutex", "[sync][unsafe]") {
  prims::Sync<int> s(42);

  std::mutex& mtx = s.mutex();

  // Manual locking
  mtx.lock();
  s.unsafe_get() = 100;
  mtx.unlock();

  REQUIRE(*s.lock() == 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// RAII guarantee tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Lock released on exception", "[sync][raii]") {
  prims::Sync<int> s(42);

  try {
    auto lock = s.lock();
    throw std::runtime_error("test");
  } catch (...) {
    // Lock should be released
  }

  // Should be able to acquire lock
  auto maybe_lock = s.try_lock();
  REQUIRE(maybe_lock.has_value());
}

TEST_CASE("Lock released when scope exits", "[sync][raii]") {
  prims::Sync<int> s(42);

  {
    auto lock = s.lock();
    *lock = 100;
  }

  // Lock should be released
  auto maybe_lock = s.try_lock();
  REQUIRE(maybe_lock.has_value());
  REQUIRE(**maybe_lock == 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// Complex type tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Sync with vector", "[sync][complex]") {
  prims::Sync<std::vector<int>> vec;

  vec.with_lock([](std::vector<int>& v) {
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
  });

  int sum = vec.with_read_lock(
      [](const std::vector<int>& v) { return std::accumulate(v.begin(), v.end(), 0); });

  REQUIRE(sum == 6);
}

TEST_CASE("Sync with map", "[sync][complex]") {
  prims::Sync<std::map<std::string, int>> map;

  map.with_lock([](std::map<std::string, int>& m) {
    m["one"] = 1;
    m["two"] = 2;
    m["three"] = 3;
  });

  auto result = map.with_read_lock([](const std::map<std::string, int>& m) -> int {
    auto it = m.find("two");
    return it != m.end() ? it->second : -1;
  });

  REQUIRE(result == 2);
}

TEST_CASE("Sync with unique_ptr", "[sync][complex]") {
  prims::Sync<std::unique_ptr<int>> ptr;

  ptr.with_lock([](std::unique_ptr<int>& p) { p = std::make_unique<int>(42); });

  int value = ptr.with_read_lock([](const std::unique_ptr<int>& p) { return p ? *p : -1; });

  REQUIRE(value == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property tests for thread safety
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SharedSync concurrent read property", "[sync][shared][property]") {
  rc::prop("concurrent readers see consistent values", []() {
    int initial = *rc::gen::arbitrary<int>();
    prims::SharedSync<int> s(initial);

    std::atomic<bool> all_match{true};
    std::atomic<int> readers_done{0};

    auto reader = [&] {
      for (int i = 0; i < 100; ++i) {
        auto lock = s.read_lock();
        if (*lock != initial) {
          all_match.store(false);
        }
      }
      readers_done.fetch_add(1);
    };

    std::thread t1(reader);
    std::thread t2(reader);
    std::thread t3(reader);

    t1.join();
    t2.join();
    t3.join();

    RC_ASSERT(all_match.load());
    RC_ASSERT(readers_done.load() == 3);
  });
}

TEST_CASE("Sync modifications are visible after lock release", "[sync][property]") {
  rc::prop("modifications visible after lock release", []() {
    std::vector<int> values = *rc::gen::nonEmpty(rc::gen::container<std::vector<int>>(
        rc::gen::arbitrary<int>()));

    prims::Sync<int> s(0);

    for (int v : values) {
      s.with_lock([v](int& stored) { stored = v; });
      RC_ASSERT(*s.lock() == v);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge case tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Self-assignment is safe", "[sync][edge]") {
  prims::Sync<int> s(42);
  s = s; // Self-assignment
  REQUIRE(*s.lock() == 42);
}

TEST_CASE("Self-swap is safe", "[sync][edge]") {
  prims::Sync<int> s(42);
  s.swap(s); // Self-swap
  REQUIRE(*s.lock() == 42);
}

TEST_CASE("Empty vector operations", "[sync][edge]") {
  prims::Sync<std::vector<int>> vec;

  auto size = vec.with_read_lock([](const std::vector<int>& v) { return v.size(); });
  REQUIRE(size == 0);

  vec.with_lock([](std::vector<int>& v) { v.clear(); });
  REQUIRE(vec.with_read_lock([](const std::vector<int>& v) { return v.empty(); }));
}

TEST_CASE("Lock move semantics", "[sync][edge]") {
  prims::Sync<int> s(42);

  auto lock1 = s.lock();
  auto lock2 = std::move(lock1);

  REQUIRE(lock2.owns_lock());
  REQUIRE(*lock2 == 42);
}
