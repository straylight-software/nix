// straylight // nix // util // tests
//
// Concurrency tests for resource pool - ISOLATED for debugging
// These tests involve blocking and threading which may hang

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <atomic>
#include <chrono>
#include <deque>
#include <random>
#include <thread>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/pool.h"
#include "nix/util/ref.h"

using namespace nix;
using namespace std::chrono_literals;

#include <future>

// ─────────────────────────────────────────────────────────────────────────────
// Test resource type
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct test_resource {
  int id;
  bool valid = true;
  static std::atomic<int> construction_count;

  test_resource() : id(construction_count++) {}
  ~test_resource() = default;

  test_resource(const test_resource&) = delete;
  test_resource& operator=(const test_resource&) = delete;
  test_resource(test_resource&&) = delete;
  test_resource& operator=(test_resource&&) = delete;
};

std::atomic<int> test_resource::construction_count{0};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Blocking tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool factory exception does not leak in use count", "[pool][exception][concurrency]") {
  std::atomic<bool> should_throw{true};

  Pool<test_resource> pool(2, [&]() -> ref<test_resource> {
    if (should_throw) {
      throw std::runtime_error("factory failed");
    }
    return make_ref<test_resource>();
  });

  // exhaust capacity with exceptions
  REQUIRE_THROWS(pool.get());
  REQUIRE_THROWS(pool.get());

  // in use count should have been decremented on exception
  should_throw = false;

  // should not block
  std::promise<void> promise;
  auto future = promise.get_future();

  std::thread t([&]() {
    auto h1 = pool.get();
    auto h2 = pool.get();
    promise.set_value();
  });

  auto status = future.wait_for(1s);
  REQUIRE(status == std::future_status::ready);
  t.join();
}

TEST_CASE("pool get blocks when capacity exhausted", "[pool][capacity][!mayfail]") {
  Pool<test_resource> pool(2, []() { return make_ref<test_resource>(); });

  auto h1 = pool.get();
  auto h2 = pool.get();

  std::atomic<bool> got_resource{false};
  std::thread waiting_thread([&]() {
    auto h3 = pool.get();
    got_resource = true;
  });

  // give the thread time to block
  std::this_thread::sleep_for(50ms);
  REQUIRE(got_resource == false);

  // release one resource
  {
    auto released = std::move(h1);
  }

  waiting_thread.join();
  REQUIRE(got_resource == true);
}

TEST_CASE("pool concurrent access respects capacity limit", "[pool][concurrency]") {
  constexpr size_t capacity = 5;
  constexpr size_t num_threads = 20;

  Pool<test_resource> pool(capacity, []() { return make_ref<test_resource>(); });

  std::atomic<size_t> concurrent_count{0};
  std::atomic<size_t> max_concurrent{0};
  std::atomic<bool> capacity_exceeded{false};

  std::vector<std::thread> threads;
  threads.reserve(num_threads);

  for (size_t i = 0; i < num_threads; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < 10; ++j) {
        auto handle = pool.get();

        size_t current = ++concurrent_count;
        if (current > capacity) {
          capacity_exceeded = true;
        }

        size_t expected = max_concurrent.load();
        while (current > expected && !max_concurrent.compare_exchange_weak(expected, current)) {
          // retry
        }

        std::this_thread::sleep_for(1ms);
        --concurrent_count;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  REQUIRE(capacity_exceeded == false);
  REQUIRE(max_concurrent <= capacity);
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrency property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool concurrent property tests", "[pool][property][concurrency]") {
  rc::prop("concurrent access never exceeds capacity", []() {
    auto capacity = *rc::gen::inRange<size_t>(2, 10);
    auto num_threads = *rc::gen::inRange<size_t>(2, 8);
    auto ops_per_thread = *rc::gen::inRange<size_t>(5, 20);

    Pool<test_resource> pool(capacity, []() { return make_ref<test_resource>(); });

    std::atomic<size_t> concurrent_count{0};
    std::atomic<bool> exceeded{false};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (size_t i = 0; i < num_threads; ++i) {
      threads.emplace_back([&]() {
        for (size_t j = 0; j < ops_per_thread; ++j) {
          auto handle = pool.get();
          size_t current = ++concurrent_count;
          if (current > capacity) {
            exceeded = true;
          }
          std::this_thread::yield();
          --concurrent_count;
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    RC_ASSERT(!exceeded);
  });

  rc::prop("pool resources are properly tracked across threads", []() {
    auto capacity = *rc::gen::inRange<size_t>(3, 10);
    auto num_threads = *rc::gen::inRange<size_t>(2, 6);

    Pool<test_resource> pool(capacity, []() { return make_ref<test_resource>(); });

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (size_t i = 0; i < num_threads; ++i) {
      threads.emplace_back([&]() {
        for (int j = 0; j < 5; ++j) {
          auto handle = pool.get();
          std::this_thread::yield();
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    // all resources should be idle now
    // count should not exceed what was actually created
    RC_ASSERT(pool.count() <= capacity);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrent fuzz test
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool concurrent fuzz test", "[pool][fuzz][concurrency]") {
  rc::prop("pool survives concurrent random operations", []() {
    auto capacity = *rc::gen::inRange<size_t>(3, 10);
    auto num_threads = *rc::gen::inRange<size_t>(2, 5);
    auto ops_per_thread = *rc::gen::inRange<size_t>(10, 30);

    Pool<test_resource> pool(
        capacity, []() { return make_ref<test_resource>(); },
        [](const ref<test_resource>& r) { return r->valid; });

    std::atomic<size_t> concurrent_count{0};
    std::atomic<bool> exceeded{false};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (size_t i = 0; i < num_threads; ++i) {
      threads.emplace_back([&, i]() {
        // thread-local random generator
        std::mt19937 rng(static_cast<unsigned>(i) + 1);
        std::deque<Pool<test_resource>::Handle> local_handles;

        for (size_t j = 0; j < ops_per_thread; ++j) {
          // randomly acquire or release
          if (local_handles.empty() || (rng() % 3 != 0)) {
            if (concurrent_count.load() < capacity) {
              auto handle = pool.get();
              size_t current = ++concurrent_count;
              if (current > capacity) {
                exceeded = true;
              }
              local_handles.push_back(std::move(handle));
            }
          } else {
            --concurrent_count;
            local_handles.pop_back();
          }

          std::this_thread::yield();
        }

        // release remaining
        while (!local_handles.empty()) {
          --concurrent_count;
          local_handles.pop_back();
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    RC_ASSERT(!exceeded);
    RC_ASSERT(concurrent_count == 0);
  });
}
