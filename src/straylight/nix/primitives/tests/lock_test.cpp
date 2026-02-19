// lock_test.cpp - Tests for mutual exclusion primitives

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "straylight/nix/primitives/lock.h"

namespace fs = std::filesystem;
using namespace straylight::nix::primitives;

// ============================================================================
// Test helpers
// ============================================================================

static fs::path test_dir;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                                                 \
  void test_##name();                                                                              \
  struct test_##name##_registrar {                                                                 \
    test_##name##_registrar() {                                                                    \
      std::cout << "Running: " #name "... " << std::flush;                                         \
      try {                                                                                        \
        test_##name();                                                                             \
        std::cout << "PASSED\n";                                                                   \
        ++tests_passed;                                                                            \
      } catch (const std::exception& e) {                                                          \
        std::cout << "FAILED: " << e.what() << "\n";                                               \
        ++tests_failed;                                                                            \
      } catch (...) {                                                                              \
        std::cout << "FAILED: unknown exception\n";                                                \
        ++tests_failed;                                                                            \
      }                                                                                            \
    }                                                                                              \
  } test_##name##_instance;                                                                        \
  void test_##name()

#define ASSERT(cond)                                                                               \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      throw std::runtime_error("Assertion failed: " #cond);                                        \
    }                                                                                              \
  } while (0)

#define ASSERT_EQ(a, b)                                                                            \
  do {                                                                                             \
    if ((a) != (b)) {                                                                              \
      throw std::runtime_error("Assertion failed: " #a " != " #b);                                 \
    }                                                                                              \
  } while (0)

// ============================================================================
// Tests
// ============================================================================

TEST(acquire_and_release) {
  auto lock_path = test_dir / "test1.lock";

  {
    auto lock = exclusive_lock::acquire(lock_path);
    ASSERT(lock.has_value());
    ASSERT(lock->is_held());
  }

  // Should be able to acquire again after release
  {
    auto lock = exclusive_lock::acquire(lock_path);
    ASSERT(lock.has_value());
  }
}

TEST(try_acquire_succeeds_when_free) {
  auto lock_path = test_dir / "test2.lock";

  auto lock = exclusive_lock::try_acquire(lock_path);
  ASSERT(lock.has_value());
  ASSERT(lock->is_held());
}

TEST(try_acquire_fails_when_held) {
  auto lock_path = test_dir / "test3.lock";

  auto lock1 = exclusive_lock::acquire(lock_path);
  ASSERT(lock1.has_value());

  // Second try_acquire should fail immediately
  auto lock2 = exclusive_lock::try_acquire(lock_path);
  ASSERT(!lock2.has_value());
  ASSERT_EQ(lock2.error(), lock_error::lock_failed);
}

TEST(move_semantics) {
  auto lock_path = test_dir / "test4.lock";

  auto lock1 = exclusive_lock::acquire(lock_path);
  ASSERT(lock1.has_value());

  // Move construct
  exclusive_lock lock2 = std::move(*lock1);
  ASSERT(lock2.is_held());
  ASSERT(!lock1->is_held());

  // Move assign
  exclusive_lock lock3 = exclusive_lock::try_acquire(test_dir / "dummy.lock").value();
  lock3 = std::move(lock2);
  ASSERT(lock3.is_held());
  ASSERT(!lock2.is_held());
}

TEST(shared_lock_multiple_readers) {
  auto lock_path = test_dir / "test5.lock";

  auto r1 = shared_lock::acquire(lock_path);
  ASSERT(r1.has_value());

  auto r2 = shared_lock::acquire(lock_path);
  ASSERT(r2.has_value());

  auto r3 = shared_lock::acquire(lock_path);
  ASSERT(r3.has_value());
}

TEST(shared_exclusive_conflict) {
  auto lock_path = test_dir / "test6.lock";

  auto shared = shared_lock::acquire(lock_path);
  ASSERT(shared.has_value());

  // Exclusive should fail while shared is held
  auto exclusive = exclusive_lock::try_acquire(lock_path);
  ASSERT(!exclusive.has_value());
}

TEST(exclusive_shared_conflict) {
  auto lock_path = test_dir / "test7.lock";

  auto exclusive = exclusive_lock::acquire(lock_path);
  ASSERT(exclusive.has_value());

  // Shared should fail while exclusive is held
  auto shared = shared_lock::try_acquire(lock_path);
  ASSERT(!shared.has_value());
}

TEST(multithread_mutual_exclusion) {
  auto lock_path = test_dir / "test8.lock";

  std::atomic<int> counter{0};
  std::atomic<int> max_concurrent{0};
  std::atomic<int> violations{0};

  constexpr int num_threads = 8;
  constexpr int iterations = 50;

  auto worker = [&]() {
    for (int i = 0; i < iterations; ++i) {
      auto lock = exclusive_lock::acquire(lock_path);
      ASSERT(lock.has_value());

      int prev = counter.fetch_add(1);
      if (prev != 0) {
        ++violations;
      }

      // Update max_concurrent
      int current = counter.load();
      int max_seen = max_concurrent.load();
      while (current > max_seen && !max_concurrent.compare_exchange_weak(max_seen, current)) {
      }

      // Brief work
      std::this_thread::sleep_for(std::chrono::microseconds(10));

      counter.fetch_sub(1);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(worker);
  }

  for (auto& t : threads) {
    t.join();
  }

  ASSERT_EQ(violations.load(), 0);
  ASSERT_EQ(counter.load(), 0);
  // max_concurrent should be 1 (mutual exclusion)
  ASSERT(max_concurrent.load() <= 1);
}

TEST(process_death_releases_lock) {
  auto lock_path = test_dir / "test9.lock";

  pid_t child = fork();
  if (child == 0) {
    // Child: acquire lock and exit without releasing
    auto lock = exclusive_lock::acquire(lock_path);
    if (!lock.has_value()) {
      _exit(1);
    }
    // Exit immediately - kernel should release flock
    _exit(0);
  }

  // Parent: wait for child
  int status;
  waitpid(child, &status, 0);
  ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  // Should be able to acquire now
  auto lock = exclusive_lock::acquire(lock_path);
  ASSERT(lock.has_value());
}

TEST(fork_does_not_inherit_ownership) {
  auto lock_path = test_dir / "test10.lock";

  auto lock = exclusive_lock::acquire(lock_path);
  ASSERT(lock.has_value());

  pid_t child = fork();
  if (child == 0) {
    // Child: lock should not be owned by us
    // Destructor should NOT release it (different PID)
    lock->release(); // This should be a no-op (wrong PID)

    // But the fd is still inherited, so flock is shared...
    // This is the tricky part. Let's verify parent still holds it.
    _exit(0);
  }

  // Parent: wait for child
  int status;
  waitpid(child, &status, 0);
  ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  // Parent should still hold the lock
  ASSERT(lock->is_held());

  // Another process should not be able to acquire
  pid_t child2 = fork();
  if (child2 == 0) {
    auto lock2 = exclusive_lock::try_acquire(lock_path);
    _exit(lock2.has_value() ? 1 : 0); // Expect failure (0)
  }

  waitpid(child2, &status, 0);
  ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

TEST(lock_guard_raii) {
  auto lock_path = test_dir / "test11.lock";

  {
    lock_guard guard(lock_path);

    // Should not be able to acquire while guard is held
    auto lock2 = exclusive_lock::try_acquire(lock_path);
    ASSERT(!lock2.has_value());
  }

  // Should be able to acquire after guard destroyed
  auto lock3 = exclusive_lock::try_acquire(lock_path);
  ASSERT(lock3.has_value());
}

TEST(stress_test) {
  auto lock_path = test_dir / "stress.lock";

  constexpr int num_processes = 4;
  constexpr int iterations = 100;

  for (int p = 0; p < num_processes; ++p) {
    pid_t child = fork();
    if (child == 0) {
      for (int i = 0; i < iterations; ++i) {
        auto lock = exclusive_lock::acquire(lock_path);
        if (!lock.has_value()) {
          _exit(1);
        }
        // Brief work
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }
      _exit(0);
    }
  }

  // Wait for all children
  int failed = 0;
  for (int p = 0; p < num_processes; ++p) {
    int status;
    wait(&status);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      ++failed;
    }
  }

  ASSERT_EQ(failed, 0);
}

// ============================================================================
// Main
// ============================================================================

int main() {
  // Create temp directory for tests
  test_dir = fs::temp_directory_path() / ("lock_test_" + std::to_string(getpid()));
  fs::create_directories(test_dir);

  std::cout << "Test directory: " << test_dir << "\n\n";

  // Tests run automatically via static initialization

  std::cout << "\n";
  std::cout << "Passed: " << tests_passed << "\n";
  std::cout << "Failed: " << tests_failed << "\n";

  // Cleanup
  fs::remove_all(test_dir);

  return tests_failed > 0 ? 1 : 0;
}
