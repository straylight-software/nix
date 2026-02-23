// straylight::nix::sync::pool tests
//
// Tests for thread-safe resource pool primitive with RAII handles.
// Property-based testing with rapidcheck for concurrency correctness.

// IMPORTANT: Catch2 v3 MUST be included BEFORE rapidcheck/catch.h
// clang-format off
#include <catch2/catch_test_macros.hpp>
// clang-format on

#include <atomic>
#include <chrono>
#include <cstddef>
#include <latch>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "../pool.h"

namespace pool = straylight::nix::sync;

// ─────────────────────────────────────────────────────────────────────────────
// Test resource type
// ─────────────────────────────────────────────────────────────────────────────

struct TestResource {
  int id;
  bool healthy = true;
  std::atomic<int>* create_count = nullptr;
  std::atomic<int>* destroy_count = nullptr;

  TestResource() : id(0) {}
  explicit TestResource(int id) : id(id) {}
  TestResource(int id, std::atomic<int>* create_count, std::atomic<int>* destroy_count)
      : id(id), create_count(create_count), destroy_count(destroy_count) {
    if (create_count) {
      create_count->fetch_add(1);
    }
  }

  ~TestResource() {
    if (destroy_count) {
      destroy_count->fetch_add(1);
    }
  }

  // Non-copyable, non-movable (like real resources)
  TestResource(const TestResource&) = delete;
  TestResource& operator=(const TestResource&) = delete;
  TestResource(TestResource&&) = delete;
  TestResource& operator=(TestResource&&) = delete;
};

// ─────────────────────────────────────────────────────────────────────────────
// Basic construction tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pool default construction", "[pool]") {
  pool::Pool<TestResource> p;
  REQUIRE(p.max_size() == 8);
  REQUIRE(p.in_use() == 0);
  REQUIRE(p.idle() == 0);
  REQUIRE(p.size() == 0);
}

TEST_CASE("Pool construction with max_size", "[pool]") {
  pool::Pool<TestResource> p(16);
  REQUIRE(p.max_size() == 16);
  REQUIRE(p.in_use() == 0);
}

TEST_CASE("Pool construction with config", "[pool]") {
  std::atomic<int> created{0};

  pool::PoolConfig<TestResource> config{
      .max_size = 4,
      .factory =
          [&created]() {
            int id = created.fetch_add(1);
            return std::make_unique<TestResource>(id);
          },
      .init_mode = pool::PoolInitMode::Lazy,
  };

  pool::Pool<TestResource> p(config);
  REQUIRE(p.max_size() == 4);
  REQUIRE(created.load() == 0); // Lazy: nothing created yet
}

TEST_CASE("Pool eager initialization", "[pool]") {
  std::atomic<int> created{0};

  pool::PoolConfig<TestResource> config{
      .max_size = 4,
      .factory =
          [&created]() {
            int id = created.fetch_add(1);
            return std::make_unique<TestResource>(id);
          },
      .init_mode = pool::PoolInitMode::Eager,
  };

  pool::Pool<TestResource> p(config);
  REQUIRE(created.load() == 4); // Eager: all created upfront
  REQUIRE(p.idle() == 4);
  REQUIRE(p.in_use() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Basic acquisition tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pool get() acquires resource", "[pool]") {
  std::atomic<int> created{0};

  pool::PoolConfig<TestResource> config{
      .max_size = 4,
      .factory =
          [&created]() {
            int id = created.fetch_add(1);
            return std::make_unique<TestResource>(id);
          },
  };

  pool::Pool<TestResource> p(config);

  {
    auto handle = p.get();
    REQUIRE(handle->id == 0);
    REQUIRE(p.in_use() == 1);
    REQUIRE(created.load() == 1);
  }

  // Resource returned to pool
  REQUIRE(p.in_use() == 0);
  REQUIRE(p.idle() == 1);
}

TEST_CASE("Pool try_get() returns resource when available", "[pool]") {
  pool::Pool<TestResource> p(4);

  auto handle = p.try_get();
  REQUIRE(handle.has_value());
  REQUIRE(p.in_use() == 1);
}

TEST_CASE("Pool try_get() returns nullopt when pool exhausted", "[pool]") {
  pool::Pool<TestResource> p(2);

  // Exhaust the pool
  auto h1 = p.get();
  auto h2 = p.get();

  REQUIRE(p.in_use() == 2);

  // try_get should fail
  auto h3 = p.try_get();
  REQUIRE_FALSE(h3.has_value());
}

TEST_CASE("Pool get() with timeout", "[pool]") {
  pool::Pool<TestResource> p(1);

  auto h1 = p.get();
  REQUIRE(p.in_use() == 1);

  // get with timeout should return nullopt
  auto h2 = p.get(std::chrono::milliseconds(10));
  REQUIRE_FALSE(h2.has_value());
}

TEST_CASE("Pool get() with timeout succeeds when released", "[pool]") {
  pool::Pool<TestResource> p(1);

  std::atomic<bool> waiter_started{false};
  std::optional<pool::Pool<TestResource>::Handle> result;
  std::thread waiter;

  // Hold the resource in a scope
  {
    auto held_resource = p.get();
    REQUIRE(p.in_use() == 1);

    // Start waiter thread that will block
    waiter = std::thread([&] {
      waiter_started.store(true);
      result = p.get(std::chrono::milliseconds(1000));
    });

    // Wait for waiter to start
    while (!waiter_started.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Give waiter time to block on semaphore
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // held_resource released when scope ends
  }

  // Wait for waiter to complete - it should succeed now that resource is released
  waiter.join();
  REQUIRE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Handle tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Handle dereference operators", "[pool][handle]") {
  std::atomic<int> created{0};

  pool::PoolConfig<TestResource> config{
      .max_size = 1,
      .factory =
          [&created]() {
            int id = created.fetch_add(1);
            return std::make_unique<TestResource>(id);
          },
  };

  pool::Pool<TestResource> p(config);

  auto handle = p.get();
  REQUIRE(handle->id == 0);
  REQUIRE((*handle).id == 0);
  REQUIRE(handle.get() != nullptr);
  REQUIRE(static_cast<bool>(handle) == true);
}

TEST_CASE("Handle move semantics", "[pool][handle]") {
  pool::Pool<TestResource> p(2);

  auto h1 = p.get();
  REQUIRE(p.in_use() == 1);

  // Move construct
  auto h2 = std::move(h1);
  REQUIRE(p.in_use() == 1); // Still just one in use

  // Move assign
  auto h3 = p.get();
  REQUIRE(p.in_use() == 2);

  h3 = std::move(h2);
  REQUIRE(p.in_use() == 1); // h3's old resource returned
}

TEST_CASE("Handle mark_bad discards resource", "[pool][handle]") {
  pool::Pool<TestResource> p(1);

  {
    auto handle = p.get();
    handle.mark_bad();
    REQUIRE(handle.is_bad() == true);
  }

  // Resource was discarded, not returned
  REQUIRE(p.idle() == 0);
  REQUIRE(p.in_use() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Resource reuse tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pool reuses resources", "[pool]") {
  std::atomic<int> created{0};

  pool::PoolConfig<TestResource> config{
      .max_size = 2,
      .factory =
          [&created]() {
            int id = created.fetch_add(1);
            return std::make_unique<TestResource>(id);
          },
  };

  pool::Pool<TestResource> p(config);

  int first_id;
  {
    auto h = p.get();
    first_id = h->id;
  }

  REQUIRE(created.load() == 1);

  {
    auto h = p.get();
    REQUIRE(h->id == first_id); // Same resource reused
  }

  REQUIRE(created.load() == 1); // No new resource created
}

// ─────────────────────────────────────────────────────────────────────────────
// Health check tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pool health check filters unhealthy resources", "[pool][health]") {
  std::atomic<int> created{0};

  pool::PoolConfig<TestResource> config{
      .max_size = 2,
      .factory =
          [&created]() {
            int id = created.fetch_add(1);
            return std::make_unique<TestResource>(id);
          },
      .health_check = [](const TestResource& r) { return r.healthy; },
  };

  pool::Pool<TestResource> p(config);

  // Get and mark unhealthy
  {
    auto h = p.get();
    h->healthy = false;
    // Don't mark_bad, let health check handle it
  }

  REQUIRE(p.idle() == 1);
  REQUIRE(created.load() == 1);

  // Get should skip unhealthy and create new
  {
    auto h = p.get();
    REQUIRE(h->id == 1); // New resource
    REQUIRE(h->healthy == true);
  }

  REQUIRE(created.load() == 2);
}

TEST_CASE("Pool flush_bad removes unhealthy from free list", "[pool][health]") {
  pool::PoolConfig<TestResource> config{
      .max_size = 4,
      .health_check = [](const TestResource& r) { return r.healthy; },
      .init_mode = pool::PoolInitMode::Eager,
  };

  pool::Pool<TestResource> p(config);
  REQUIRE(p.idle() == 4);

  // Mark some as unhealthy
  {
    auto h1 = p.get();
    auto h2 = p.get();
    h1->healthy = false;
    h2->healthy = false;
  }

  REQUIRE(p.idle() == 4);

  p.flush_bad();
  REQUIRE(p.idle() == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Clear tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pool clear removes all idle resources", "[pool]") {
  pool::PoolConfig<TestResource> config{
      .max_size = 4,
      .init_mode = pool::PoolInitMode::Eager,
  };

  pool::Pool<TestResource> p(config);
  REQUIRE(p.idle() == 4);

  auto cleared = p.clear();
  REQUIRE(cleared.size() == 4);
  REQUIRE(p.idle() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrent acquisition tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pool concurrent acquisition respects max_size", "[pool][concurrent]") {
  constexpr std::size_t max_size = 4;
  constexpr std::size_t num_threads = 16;
  constexpr std::size_t iterations = 100;

  pool::Pool<TestResource> p(max_size);

  std::atomic<std::size_t> max_concurrent{0};
  std::atomic<std::size_t> current_concurrent{0};
  std::latch start_latch(num_threads);

  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < num_threads; ++t) {
    threads.emplace_back([&] {
      start_latch.arrive_and_wait();

      for (std::size_t i = 0; i < iterations; ++i) {
        auto handle = p.get();

        std::size_t current = current_concurrent.fetch_add(1) + 1;
        std::size_t expected = max_concurrent.load();
        while (current > expected && !max_concurrent.compare_exchange_weak(expected, current)) {
        }

        // Brief work
        std::this_thread::sleep_for(std::chrono::microseconds(10));

        current_concurrent.fetch_sub(1);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Should never exceed max_size
  REQUIRE(max_concurrent.load() <= max_size);
  REQUIRE(p.in_use() == 0);
}

TEST_CASE("Pool concurrent try_get handles contention", "[pool][concurrent]") {
  constexpr std::size_t max_size = 2;
  constexpr std::size_t num_threads = 8;
  constexpr std::size_t iterations = 50;

  pool::Pool<TestResource> p(max_size);

  std::atomic<std::size_t> successes{0};
  std::atomic<std::size_t> failures{0};
  std::latch start_latch(num_threads);

  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < num_threads; ++t) {
    threads.emplace_back([&] {
      start_latch.arrive_and_wait();

      for (std::size_t i = 0; i < iterations; ++i) {
        if (auto handle = p.try_get()) {
          successes.fetch_add(1);
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        } else {
          failures.fetch_add(1);
          std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Should have both successes and failures with contention
  REQUIRE(successes.load() > 0);
  REQUIRE(failures.load() > 0);
  REQUIRE(p.in_use() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pool properties: in_use + idle <= max_size", "[pool][property]") {
  rc::prop("in_use + idle never exceeds max_size", []() {
    auto max_size = *rc::gen::inRange<std::size_t>(1, 32);
    pool::Pool<TestResource> p(max_size);

    auto num_handles = *rc::gen::inRange<std::size_t>(0, max_size + 5);
    std::vector<std::optional<pool::Pool<TestResource>::Handle>> handles;

    for (std::size_t i = 0; i < num_handles; ++i) {
      handles.push_back(p.try_get());
    }

    RC_ASSERT(p.in_use() + p.idle() <= p.max_size());
    RC_ASSERT(p.in_use() <= p.max_size());
  });
}

TEST_CASE("Pool properties: resources returned on scope exit", "[pool][property]") {
  rc::prop("all resources returned after handles destroyed", []() {
    auto max_size = *rc::gen::inRange<std::size_t>(1, 16);
    pool::Pool<TestResource> p(max_size);

    auto num_acquires = *rc::gen::inRange<std::size_t>(1, max_size);

    {
      std::vector<pool::Pool<TestResource>::Handle> handles;
      for (std::size_t i = 0; i < num_acquires; ++i) {
        handles.push_back(p.get());
      }
      RC_ASSERT(p.in_use() == num_acquires);
    }

    RC_ASSERT(p.in_use() == 0);
    RC_ASSERT(p.idle() == num_acquires);
  });
}

TEST_CASE("Pool properties: created count matches actual creations", "[pool][property]") {
  rc::prop("created() tracks total resources created", []() {
    auto max_size = *rc::gen::inRange<std::size_t>(1, 8);
    std::atomic<int> factory_calls{0};

    pool::PoolConfig<TestResource> config{
        .max_size = max_size,
        .factory =
            [&factory_calls]() {
              factory_calls.fetch_add(1);
              return std::make_unique<TestResource>();
            },
    };

    pool::Pool<TestResource> p(config);

    auto num_acquires = *rc::gen::inRange<std::size_t>(1, max_size * 2);

    for (std::size_t i = 0; i < num_acquires; ++i) {
      auto h = p.get();
    }

    RC_ASSERT(p.created() == static_cast<std::size_t>(factory_calls.load()));
    // Should create at most max_size resources (reuse kicks in)
    RC_ASSERT(p.created() <= max_size);
  });
}

TEST_CASE("Pool properties: mark_bad prevents reuse", "[pool][property]") {
  rc::prop("marked bad resources are not returned to pool", []() {
    auto max_size = *rc::gen::inRange<std::size_t>(2, 8);
    pool::Pool<TestResource> p(max_size);

    auto num_bad = *rc::gen::inRange<std::size_t>(1, max_size);

    std::size_t initial_idle = p.idle();

    for (std::size_t i = 0; i < num_bad; ++i) {
      auto h = p.get();
      h.mark_bad();
    }

    // No resources added to idle (all discarded)
    RC_ASSERT(p.idle() == initial_idle);
    RC_ASSERT(p.in_use() == 0);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge case tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pool edge case: single resource pool", "[pool][edge]") {
  pool::Pool<TestResource> p(1);

  auto h1 = p.get();
  REQUIRE(p.in_use() == 1);
  REQUIRE(p.try_get() == std::nullopt);
}

TEST_CASE("Pool edge case: rapid acquire/release cycles", "[pool][edge]") {
  pool::Pool<TestResource> p(2);

  for (int i = 0; i < 1000; ++i) {
    auto h = p.get();
    // Immediately released
  }

  REQUIRE(p.in_use() == 0);
  REQUIRE(p.created() <= 2); // Resources should be reused
}

TEST_CASE("Pool edge case: all resources marked bad", "[pool][edge]") {
  pool::Pool<TestResource> p(3);

  // Create and mark all bad
  for (int i = 0; i < 3; ++i) {
    auto h = p.get();
    h.mark_bad();
  }

  REQUIRE(p.idle() == 0);

  // Should still be able to create new
  auto h = p.get();
  REQUIRE(h.get() != nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Destruction safety tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pool destruction order is safe", "[pool][destruction]") {
  std::atomic<int> factory_calls{0};
  std::atomic<int> destroyed{0};

  {
    pool::PoolConfig<TestResource> config{
        .max_size = 4,
        .factory =
            [&]() {
              int id = factory_calls.fetch_add(1);
              // Pass nullptr for create_count to avoid double-counting
              return std::make_unique<TestResource>(id, nullptr, &destroyed);
            },
        .init_mode = pool::PoolInitMode::Eager,
    };

    pool::Pool<TestResource> p(config);
    REQUIRE(factory_calls.load() == 4);

    // Acquire and release some
    {
      auto h1 = p.get();
      auto h2 = p.get();
    }

    REQUIRE(destroyed.load() == 0); // Resources still in pool
  }

  // Pool destroyed, all resources should be destroyed
  REQUIRE(destroyed.load() == 4);
}
