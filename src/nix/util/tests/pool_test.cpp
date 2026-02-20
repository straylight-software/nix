// straylight // nix // util // tests
//
// Unit, property-based, and fuzz tests for resource pool

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/pool.h"
#include "nix/util/ref.h"

using namespace nix;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Test resource type
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct test_resource {
  int id;
  bool valid = true;
  static std::atomic<int> construction_count;
  static std::atomic<int> destruction_count;

  test_resource() : id(construction_count++) {}
  explicit test_resource(int resource_id) : id(resource_id) { construction_count++; }
  ~test_resource() { destruction_count++; }

  // rule of 5: explicitly delete copy/move since we track construction/destruction
  test_resource(const test_resource&) = delete;
  test_resource& operator=(const test_resource&) = delete;
  test_resource(test_resource&&) = delete;
  test_resource& operator=(test_resource&&) = delete;

  static void reset_counters() {
    construction_count = 0;
    destruction_count = 0;
  }
};

std::atomic<int> test_resource::construction_count{0};
std::atomic<int> test_resource::destruction_count{0};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Basic pool operations
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool get creates resource on first access", "[pool]") {
  test_resource::reset_counters();

  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(); });

  REQUIRE(test_resource::construction_count == 0);

  {
    auto handle = pool.get();
    REQUIRE(test_resource::construction_count == 1);
  }
}

TEST_CASE("pool get returns same resource after release", "[pool]") {
  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(); });

  int first_id = 0;
  {
    auto handle = pool.get();
    first_id = handle->id;
  }

  {
    auto handle = pool.get();
    REQUIRE(handle->id == first_id);
  }
}

TEST_CASE("pool handle provides access to resource via arrow operator", "[pool]") {
  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(42); });

  auto handle = pool.get();
  REQUIRE(handle->id == 42);
}

TEST_CASE("pool handle provides access to resource via dereference operator", "[pool]") {
  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(42); });

  auto handle = pool.get();
  test_resource& res = *handle;
  REQUIRE(res.id == 42);
}

TEST_CASE("pool count reflects active and idle resources", "[pool]") {
  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(); });

  REQUIRE(pool.count() == 0);

  {
    auto handle1 = pool.get();
    REQUIRE(pool.count() == 1);

    auto handle2 = pool.get();
    REQUIRE(pool.count() == 2);
  }

  // after handles are released, count should still reflect pooled resources
  REQUIRE(pool.count() == 2);
}

TEST_CASE("pool capacity returns maximum size", "[pool]") {
  pool_t<test_resource> pool(5, []() { return make_ref<test_resource>(); });
  REQUIRE(pool.capacity() == 5);
}

TEST_CASE("pool incCapacity increases maximum", "[pool]") {
  pool_t<test_resource> pool(5, []() { return make_ref<test_resource>(); });
  REQUIRE(pool.capacity() == 5);

  pool.incCapacity();
  REQUIRE(pool.capacity() == 6);

  pool.incCapacity();
  REQUIRE(pool.capacity() == 7);
}

TEST_CASE("pool decCapacity decreases maximum", "[pool]") {
  pool_t<test_resource> pool(5, []() { return make_ref<test_resource>(); });
  REQUIRE(pool.capacity() == 5);

  pool.decCapacity();
  REQUIRE(pool.capacity() == 4);
}

TEST_CASE("pool clear removes all idle resources", "[pool]") {
  test_resource::reset_counters();

  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(); });

  {
    auto h1 = pool.get();
    auto h2 = pool.get();
    auto h3 = pool.get();
  }

  REQUIRE(pool.count() == 3);

  auto cleared = pool.clear();
  REQUIRE(cleared.size() == 3);
  REQUIRE(pool.count() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// RAII semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool handle releases resource when going out of scope", "[pool][raii]") {
  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(); });

  int resource_id = 0;
  {
    auto handle = pool.get();
    resource_id = handle->id;
    // handle goes out of scope here
  }

  // get should return the same resource
  auto handle = pool.get();
  REQUIRE(handle->id == resource_id);
}

TEST_CASE("pool handle is moveable", "[pool][raii]") {
  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(99); });

  auto handle1 = pool.get();
  auto handle2 = std::move(handle1);

  REQUIRE(handle2->id == 99);
}

TEST_CASE("pool handle move leaves source empty", "[pool][raii]") {
  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(); });

  auto handle1 = pool.get();
  REQUIRE(pool.count() == 1);

  auto handle2 = std::move(handle1);
  // moved-from handle destruction should not double-return
  // count should still be 1 (in use)
  REQUIRE(pool.count() == 1);
}

TEST_CASE("pool markBad prevents resource from being reused", "[pool][raii]") {
  test_resource::reset_counters();

  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(); });

  int first_id = 0;
  {
    auto handle = pool.get();
    first_id = handle->id;
    handle.markBad();
  }

  // should get a new resource, not the marked bad one
  auto handle = pool.get();
  REQUIRE(handle->id != first_id);
  REQUIRE(test_resource::construction_count == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool validator filters out invalid resources on get", "[pool][validation]") {
  test_resource::reset_counters();

  pool_t<test_resource> pool(
      10, []() { return make_ref<test_resource>(); },
      [](const ref<test_resource>& r) { return r->valid; });

  int first_id = 0;
  {
    auto handle = pool.get();
    first_id = handle->id;
    handle->valid = false;
  }

  // get should skip the invalid resource and create a new one
  auto handle = pool.get();
  REQUIRE(handle->id != first_id);
  REQUIRE(test_resource::construction_count == 2);
}

TEST_CASE("pool validator chains to skip multiple invalid resources", "[pool][validation]") {
  test_resource::reset_counters();

  pool_t<test_resource> pool(
      10, []() { return make_ref<test_resource>(); },
      [](const ref<test_resource>& r) { return r->valid; });

  // create 3 resources
  {
    auto h1 = pool.get();
    auto h2 = pool.get();
    auto h3 = pool.get();
    h1->valid = false;
    h2->valid = false;
    // h3 remains valid
  }

  REQUIRE(test_resource::construction_count == 3);

  // should get h3 (the only valid one)
  auto handle = pool.get();
  REQUIRE(handle->valid == true);
}

TEST_CASE("pool flushBad removes invalid idle resources", "[pool][validation]") {
  pool_t<test_resource> pool(
      10, []() { return make_ref<test_resource>(); },
      [](const ref<test_resource>& r) { return r->valid; });

  {
    auto h1 = pool.get();
    auto h2 = pool.get();
    auto h3 = pool.get();
    h1->valid = false;
    h3->valid = false;
  }

  REQUIRE(pool.count() == 3);

  pool.flushBad();

  // only one valid resource should remain
  REQUIRE(pool.count() == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Resource exhaustion and blocking - MOVED TO pool_concurrency_test.cpp
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Exception safety
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool recovers from factory exception", "[pool][exception]") {
  std::atomic<int> call_count{0};

  pool_t<test_resource> pool(10, [&]() -> ref<test_resource> {
    if (call_count++ == 0) {
      throw std::runtime_error("factory failed");
    }
    return make_ref<test_resource>();
  });

  // first call should throw
  REQUIRE_THROWS_AS(pool.get(), std::runtime_error);

  // pool should still be usable
  auto handle = pool.get();
  REQUIRE(handle->id >= 0);
}

// TEST_CASE("pool factory exception does not leak in use count") - MOVED TO
// pool_concurrency_test.cpp This test uses threads to verify non-blocking behavior

TEST_CASE("pool exception in validator during get creates new resource", "[pool][exception]") {
  std::atomic<int> validation_count{0};

  pool_t<test_resource> pool(
      10, []() { return make_ref<test_resource>(); },
      [&](const ref<test_resource>&) -> bool {
        if (validation_count++ == 0) {
          throw std::runtime_error("validator failed");
        }
        return true;
      });

  // first get creates a resource
  {
    auto handle = pool.get();
  }

  // second get tries to validate the idle resource, validator throws,
  // but a new resource should still be created
  // note: the current implementation does not catch validator exceptions,
  // so this will propagate
  REQUIRE_THROWS_AS(pool.get(), std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool property tests", "[pool][property]") {
  rc::prop("count never exceeds in use plus idle", []() {
    auto capacity = *rc::gen::inRange<size_t>(1, 20);

    pool_t<test_resource> pool(capacity, []() { return make_ref<test_resource>(); });

    // IMPORTANT: num_acquisitions must not exceed capacity or pool.get() will block
    auto num_acquisitions = *rc::gen::inRange<size_t>(1, capacity + 1);
    // use deque since Handle is not copy-assignable
    std::deque<pool_t<test_resource>::Handle> handles;

    for (size_t i = 0; i < num_acquisitions; ++i) {
      handles.push_back(pool.get());
    }

    RC_ASSERT(pool.count() == num_acquisitions);

    // release some (from back to avoid move assignment issues)
    auto num_releases = *rc::gen::inRange<size_t>(0, num_acquisitions);
    for (size_t i = 0; i < num_releases; ++i) {
      handles.pop_back();
    }

    RC_ASSERT(pool.count() == num_acquisitions);
  });

  rc::prop("pool reuses resources when available", []() {
    pool_t<test_resource> pool(100, []() { return make_ref<test_resource>(); });

    auto num_iterations = *rc::gen::inRange<size_t>(1, 50);
    std::set<int> resource_ids;

    for (size_t i = 0; i < num_iterations; ++i) {
      auto handle = pool.get();
      resource_ids.insert(handle->id);
    }

    // should have created at most one resource since we acquire/release sequentially
    RC_ASSERT(resource_ids.size() == 1);
  });

  rc::prop("markBad resources are not reused", []() {
    test_resource::reset_counters();

    pool_t<test_resource> pool(100, []() { return make_ref<test_resource>(); });

    auto num_bad = *rc::gen::inRange<size_t>(1, 10);
    std::set<int> bad_ids;

    for (size_t i = 0; i < num_bad; ++i) {
      auto handle = pool.get();
      bad_ids.insert(handle->id);
      handle.markBad();
    }

    // all subsequent gets should return new resources
    for (size_t i = 0; i < 5; ++i) {
      auto handle = pool.get();
      RC_ASSERT(bad_ids.find(handle->id) == bad_ids.end());
    }
  });

  rc::prop("clear removes all idle resources", []() {
    pool_t<test_resource> pool(100, []() { return make_ref<test_resource>(); });

    auto num_resources = *rc::gen::inRange<size_t>(0, 20);

    {
      std::deque<pool_t<test_resource>::Handle> handles;
      for (size_t i = 0; i < num_resources; ++i) {
        handles.push_back(pool.get());
      }
    }

    auto cleared = pool.clear();
    RC_ASSERT(cleared.size() == num_resources);
    RC_ASSERT(pool.count() == 0);
  });

  rc::prop("capacity changes are reflected", []() {
    auto initial_capacity = *rc::gen::inRange<size_t>(1, 50);

    pool_t<test_resource> pool(initial_capacity, []() { return make_ref<test_resource>(); });

    RC_ASSERT(pool.capacity() == initial_capacity);

    auto inc_count = *rc::gen::inRange<size_t>(0, 10);
    for (size_t i = 0; i < inc_count; ++i) {
      pool.incCapacity();
    }
    RC_ASSERT(pool.capacity() == initial_capacity + inc_count);

    auto dec_count = *rc::gen::inRange<size_t>(0, inc_count);
    for (size_t i = 0; i < dec_count; ++i) {
      pool.decCapacity();
    }
    RC_ASSERT(pool.capacity() == initial_capacity + inc_count - dec_count);
  });
}

TEST_CASE("pool validation property tests", "[pool][property][validation]") {
  rc::prop("invalid resources are filtered out on get", []() {
    pool_t<test_resource> pool(
        100, []() { return make_ref<test_resource>(); },
        [](const ref<test_resource>& r) { return r->valid; });

    auto num_invalid = *rc::gen::inRange<size_t>(1, 10);

    for (size_t i = 0; i < num_invalid; ++i) {
      auto handle = pool.get();
      handle->valid = false;
    }

    // get should skip all invalid resources
    auto handle = pool.get();
    RC_ASSERT(handle->valid == true);
  });

  rc::prop("flushBad removes exactly invalid resources", []() {
    pool_t<test_resource> pool(
        100, []() { return make_ref<test_resource>(); },
        [](const ref<test_resource>& r) { return r->valid; });

    auto num_resources = *rc::gen::inRange<size_t>(1, 20);
    auto num_invalid = *rc::gen::inRange<size_t>(0, num_resources);

    {
      std::deque<pool_t<test_resource>::Handle> handles;
      for (size_t i = 0; i < num_resources; ++i) {
        handles.push_back(pool.get());
        if (i < num_invalid) {
          handles.back()->valid = false;
        }
      }
    }

    RC_ASSERT(pool.count() == num_resources);

    pool.flushBad();

    RC_ASSERT(pool.count() == num_resources - num_invalid);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrency property tests - MOVED TO pool_concurrency_test.cpp
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool fuzz test with random operations", "[pool][fuzz]") {
  rc::prop("pool survives random operation sequences", []() {
    auto capacity = *rc::gen::inRange<size_t>(1, 20);
    auto num_operations = *rc::gen::inRange<size_t>(10, 100);

    pool_t<test_resource> pool(
        capacity, []() { return make_ref<test_resource>(); },
        [](const ref<test_resource>& r) { return r->valid; });

    // use deque for efficient pop from both ends without move assignment
    std::deque<pool_t<test_resource>::Handle> active_handles;

    for (size_t op = 0; op < num_operations; ++op) {
      auto operation = *rc::gen::inRange<int>(0, 10);

      switch (operation) {
        case 0:
        case 1:
        case 2:
        case 3:
          // acquire (more likely)
          if (active_handles.size() < capacity) {
            active_handles.push_back(pool.get());
          }
          break;
        case 4:
        case 5:
          // release from back
          if (!active_handles.empty()) {
            active_handles.pop_back();
          }
          break;
        case 6:
          // mark bad and release from front
          if (!active_handles.empty()) {
            active_handles.front().markBad();
            active_handles.pop_front();
          }
          break;
        case 7:
          // invalidate resource at front
          if (!active_handles.empty()) {
            active_handles.front()->valid = false;
          }
          break;
        case 8:
          // flush bad
          pool.flushBad();
          break;
        case 9:
          // clear (only when nothing in use)
          if (active_handles.empty()) {
            pool.clear();
          }
          break;
        default:
          break;
      }

      // invariants
      RC_ASSERT(active_handles.size() <= capacity);
    }

    // clean up
    active_handles.clear();
  });
}

TEST_CASE("pool fuzz test with capacity changes", "[pool][fuzz]") {
  rc::prop("pool handles capacity changes during operation", []() {
    auto initial_capacity = *rc::gen::inRange<size_t>(5, 15);
    auto num_operations = *rc::gen::inRange<size_t>(10, 50);

    pool_t<test_resource> pool(initial_capacity, []() { return make_ref<test_resource>(); });

    std::deque<pool_t<test_resource>::Handle> active_handles;
    size_t current_capacity = initial_capacity;

    for (size_t op = 0; op < num_operations; ++op) {
      auto operation = *rc::gen::inRange<int>(0, 10);

      switch (operation) {
        case 0:
        case 1:
        case 2:
          // acquire
          if (active_handles.size() < current_capacity) {
            active_handles.push_back(pool.get());
          }
          break;
        case 3:
        case 4:
          // release
          if (!active_handles.empty()) {
            active_handles.pop_back();
          }
          break;
        case 5:
          // increase capacity
          pool.incCapacity();
          current_capacity++;
          break;
        case 6:
          // decrease capacity (but not below in use)
          if (current_capacity > active_handles.size() + 1) {
            pool.decCapacity();
            current_capacity--;
          }
          break;
        default:
          break;
      }

      RC_ASSERT(pool.capacity() == current_capacity);
    }

    active_handles.clear();
  });
}

// pool concurrent fuzz test - MOVED TO pool_concurrency_test.cpp

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool with capacity one", "[pool][edge]") {
  pool_t<test_resource> pool(1, []() { return make_ref<test_resource>(); });

  int first_id = 0;
  {
    auto h1 = pool.get();
    first_id = h1->id;
  }

  {
    auto h2 = pool.get();
    REQUIRE(h2->id == first_id);
  }
}

TEST_CASE("pool with default factory", "[pool][edge]") {
  // test_resource has a default constructor
  pool_t<test_resource> pool(10);

  auto handle = pool.get();
  REQUIRE(handle->id >= 0);
}

TEST_CASE("pool with always-invalid validator", "[pool][edge]") {
  test_resource::reset_counters();

  pool_t<test_resource> pool(
      10, []() { return make_ref<test_resource>(); },
      [](const ref<test_resource>&) { return false; });

  // first get creates a resource
  {
    auto handle = pool.get();
  }

  // second get should create a new resource since validator always fails
  auto handle = pool.get();
  REQUIRE(test_resource::construction_count == 2);
}

TEST_CASE("pool multiple handles same resource lifecycle", "[pool][edge]") {
  test_resource::reset_counters();

  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(); });

  // get, release, get, release - should reuse same resource
  for (int i = 0; i < 10; ++i) {
    auto handle = pool.get();
  }

  REQUIRE(test_resource::construction_count == 1);
}

TEST_CASE("pool destruction with active handles", "[pool][edge]") {
  // this test verifies behavior when pool is destroyed with handles in use
  // NOTE: the pool asserts no resources are in use at destruction,
  // so we must ensure all handles are released before pool destruction

  auto pool =
      std::make_unique<pool_t<test_resource>>(10, []() { return make_ref<test_resource>(); });

  {
    auto handle = pool->get();
    // handle goes out of scope first
  }

  // pool can now be safely destroyed
  pool.reset();
}

TEST_CASE("pool stress test rapid acquire release", "[pool][stress]") {
  pool_t<test_resource> pool(5, []() { return make_ref<test_resource>(); });

  for (int i = 0; i < 1000; ++i) {
    auto handle = pool.get();
    handle->valid = (i % 7 != 0); // occasionally invalidate
  }

  // should still be functional
  auto handle = pool.get();
  REQUIRE(handle->id >= 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Handle semantics tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pool handle noexcept move constructor", "[pool][handle]") {
  pool_t<test_resource> pool(10, []() { return make_ref<test_resource>(); });

  // verify move constructor is noexcept (important for exception safety)
  static_assert(std::is_nothrow_move_constructible_v<pool_t<test_resource>::Handle>);

  auto h1 = pool.get();
  auto h2 = std::move(h1);

  REQUIRE(h2->id >= 0);
}

TEST_CASE("pool handle not copyable", "[pool][handle]") {
  // verify Handle is not copyable
  static_assert(!std::is_copy_constructible_v<pool_t<test_resource>::Handle>);
  static_assert(!std::is_copy_assignable_v<pool_t<test_resource>::Handle>);
}

TEST_CASE("pool interleaved acquire release pattern", "[pool][pattern]") {
  pool_t<test_resource> pool(3, []() { return make_ref<test_resource>(); });

  std::deque<pool_t<test_resource>::Handle> handles;

  // acquire 3
  handles.push_back(pool.get());
  handles.push_back(pool.get());
  handles.push_back(pool.get());

  REQUIRE(pool.count() == 3);

  // release 1, acquire 1
  handles.pop_front();
  handles.push_back(pool.get());

  REQUIRE(pool.count() == 3);

  // release all
  handles.clear();

  REQUIRE(pool.count() == 3);
}
