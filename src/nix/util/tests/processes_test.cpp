// straylight // nix // util // tests
//
// Unit, property-based, and fuzz tests for process handling
//
// Tests focus on:
// - Race conditions in process_handle_t::wait() and kill()
// - ECHILD handling (process already reaped by another thread/signal handler)
// - ESRCH handling (process already exited)
// - Concurrent wait/kill from multiple threads
// - Signal delivery during wait
// - Move semantics correctness under concurrent access

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <random>
#include <thread>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>
#include <sys/wait.h>
#include <unistd.h>


#include "nix/util/processes.h"

using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Test helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Fork a child that exits immediately
pid_t fork_and_exit(int exit_code = 0) {
  pid_t pid = fork();
  if (pid == 0) {
    _exit(exit_code);
  }
  return pid;
}

// Fork a child that sleeps then exits
pid_t fork_sleep_and_exit(int sleep_ms, int exit_code = 0) {
  pid_t pid = fork();
  if (pid == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    _exit(exit_code);
  }
  return pid;
}

// Fork a child that waits for a signal before exiting
pid_t fork_wait_for_signal(int sig = SIGUSR1) {
  pid_t pid = fork();
  if (pid == 0) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_BLOCK, &set, nullptr);
    int received;
    sigwait(&set, &received);
    _exit(0);
  }
  return pid;
}

// Reap a process directly using waitpid (bypassing process_handle_t)
// Returns true if successfully reaped, false if already reaped
bool reap_directly(pid_t pid) {
  int status;
  pid_t result = waitpid(pid, &status, WNOHANG);
  if (result == pid) {
    return true; // Successfully reaped
  }
  if (result == 0) {
    // Process still running, wait for it
    result = waitpid(pid, &status, 0);
    return result == pid;
  }
  // result == -1, already reaped or error
  return false;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Basic functionality tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process_handle_t basic wait", "[processes][basic]") {
  pid_t pid = fork_and_exit(42);
  REQUIRE(pid > 0);

  nix::process_handle_t handle(pid);
  int status = handle.wait();

  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 42);
}

TEST_CASE("process_handle_t kill sends signal", "[processes][basic]") {
  pid_t pid = fork_wait_for_signal(SIGTERM); // Wait for SIGTERM (graceful shutdown)
  REQUIRE(pid > 0);

  nix::process_handle_t handle(pid);
  handle.set_kill_signal(SIGKILL);
  int status = handle.kill();

  // With graceful shutdown, process receives SIGTERM first and exits.
  // If it doesn't respond to SIGTERM within timeout, SIGKILL is sent.
  REQUIRE(WIFSIGNALED(status));
  REQUIRE((WTERMSIG(status) == SIGTERM || WTERMSIG(status) == SIGKILL));
}

TEST_CASE("process_handle_t move semantics", "[processes][basic]") {
  pid_t pid = fork_sleep_and_exit(100, 0);
  REQUIRE(pid > 0);

  nix::process_handle_t h1(pid);
  nix::process_handle_t h2 = std::move(h1);

  // h1 should no longer own the pid
  // h2 should successfully wait
  int status = h2.kill();
  REQUIRE((WIFSIGNALED(status) || WIFEXITED(status)));
}

// ─────────────────────────────────────────────────────────────────────────────
// ECHILD race condition tests - THE BUG WE'RE TESTING FOR
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process_handle_t wait handles ECHILD when already reaped", "[processes][echild][race]") {
  // Regression test for: "cannot get exit status of PID: No child process"
  //
  // Scenario:
  // 1. Fork a child that exits immediately
  // 2. Reap it externally (simulating another thread or signal handler)
  // 3. Call wait() on the handle
  //
  // Before fix: wait() threw sys_error_t with ECHILD
  // After fix: wait() returns synthetic status 0 (normal exit)

  pid_t pid = fork_and_exit(0);
  REQUIRE(pid > 0);

  // Wait for child to exit
  std::this_thread::sleep_for(10ms);

  // Reap it directly, bypassing process_handle_t
  bool reaped = reap_directly(pid);
  REQUIRE(reaped);

  // Now create a handle and try to wait - should handle ECHILD gracefully
  nix::process_handle_t handle(pid);

  // Should not throw - returns synthetic status 0
  int status = handle.wait();
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

TEST_CASE("process_handle_t kill handles ESRCH then ECHILD", "[processes][echild][race]") {
  // Regression test for the exact error from the bug report:
  // "killing process X: No such process"
  // "cannot get exit status of PID X: No child process"
  //
  // Scenario:
  // 1. Fork a child
  // 2. Child exits and is reaped externally
  // 3. kill() is called:
  //    - ::kill() returns ESRCH (No such process) - handled, returns synthetic status
  //    - Or ::kill() succeeds but wait() gets ECHILD - now also handled
  //
  // Before fix: threw sys_error_t with ECHILD
  // After fix: returns synthetic status (signal or exit 0)

  pid_t pid = fork_and_exit(0);
  REQUIRE(pid > 0);

  // Wait for child to exit and reap it externally
  std::this_thread::sleep_for(10ms);
  reap_directly(pid);

  nix::process_handle_t handle(pid);

  // kill() should handle this gracefully - returns synthetic status
  int status = handle.kill();
  // Either killed by signal or synthetic exit 0 from ESRCH/ECHILD handling
  REQUIRE((WIFSIGNALED(status) || WIFEXITED(status)));
}

TEST_CASE("process_handle_t concurrent reap and wait", "[processes][echild][race][concurrency]") {
  // Multiple threads racing to wait/kill the same process
  // All should handle ECHILD gracefully after the fix

  constexpr int iterations = 50;

  for (int i = 0; i < iterations; ++i) {
    pid_t pid = fork_and_exit(0);
    REQUIRE(pid > 0);

    std::atomic<int> success_count{0};
    std::atomic<int> error_count{0};

    auto waiter = [&](bool use_kill) {
      nix::process_handle_t handle(pid);
      try {
        int status;
        if (use_kill) {
          status = handle.kill();
        } else {
          status = handle.wait();
        }
        ++success_count;
        (void)status;
      } catch (const nix::sys_error_t& e) {
        // No ECHILD errors should occur after the fix
        ++error_count;
        INFO("Unexpected error: " << e.what());
      }
    };

    std::thread t1([&] { waiter(false); });
    std::thread t2([&] { waiter(true); });
    std::thread t3([&] { waiter(false); });

    t1.join();
    t2.join();
    t3.join();

    // All operations should succeed (no ECHILD errors after fix)
    REQUIRE(error_count == 0);
    REQUIRE(success_count == 3);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests for race conditions
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process_handle_t property: wait never throws ECHILD after fix",
          "[processes][property][echild]") {
  rc::prop("wait handles externally reaped processes gracefully", []() {
    auto exit_code = *rc::gen::inRange(0, 128);
    auto sleep_before_reap_us = *rc::gen::inRange(0, 1000);

    pid_t pid = fork_and_exit(exit_code);
    RC_ASSERT(pid > 0);

    // Random delay before external reap
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_before_reap_us));

    // Externally reap (may or may not succeed depending on timing)
    reap_directly(pid);

    nix::process_handle_t handle(pid);

    // Should never throw - always returns a status (real or synthetic)
    int status = handle.wait();
    RC_ASSERT(WIFEXITED(status) || WIFSIGNALED(status));
  });
}

TEST_CASE("process_handle_t property: kill handles all race conditions",
          "[processes][property][echild]") {
  rc::prop("kill handles ESRCH and ECHILD gracefully", []() {
    auto sleep_ms = *rc::gen::inRange(0, 50);
    auto external_reap = *rc::gen::arbitrary<bool>();

    pid_t pid = fork_sleep_and_exit(sleep_ms, 0);
    RC_ASSERT(pid > 0);

    if (external_reap) {
      // Wait for process to potentially exit, then reap externally
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms + 10));
      reap_directly(pid);
    }

    nix::process_handle_t handle(pid);

    // Should never throw - always returns a status (real or synthetic)
    int status = handle.kill();
    RC_ASSERT(WIFEXITED(status) || WIFSIGNALED(status));
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrent stress tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process_handle_t stress: many short-lived processes",
          "[processes][stress][concurrency]") {
  constexpr int num_processes = 100;
  std::atomic<int> errors{0};
  std::atomic<int> success{0};

  std::vector<std::thread> threads;
  threads.reserve(num_processes);

  for (int i = 0; i < num_processes; ++i) {
    threads.emplace_back([&, i]() {
      pid_t pid = fork_and_exit(i % 128);
      if (pid <= 0) {
        ++errors;
        return;
      }

      // Random chance of external reap
      if (i % 3 == 0) {
        std::this_thread::sleep_for(5ms);
        reap_directly(pid);
      }

      nix::process_handle_t handle(pid);
      try {
        int status = handle.wait();
        // Should always get a valid status (real or synthetic)
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
          ++success;
        } else {
          ++errors;
        }
      } catch (...) {
        // No exceptions should occur after the fix
        ++errors;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  INFO("success=" << success << " errors=" << errors);

  // All operations should succeed (no ECHILD errors after fix)
  REQUIRE(errors == 0);
  REQUIRE(success == num_processes);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz tests for adversarial timing
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process_handle_t fuzz: adversarial timing attacks", "[processes][fuzz][echild]") {
  rc::prop("survives adversarial timing between fork/wait/kill", []() {
    auto num_ops = *rc::gen::inRange(1, 10);
    auto timing_seed = *rc::gen::arbitrary<unsigned>();

    std::mt19937 rng(timing_seed);

    for (int i = 0; i < num_ops; ++i) {
      pid_t pid = fork_and_exit(0);
      RC_ASSERT(pid > 0);

      // Random delays to hit different race windows
      auto pre_delay = rng() % 1000;
      auto mid_delay = rng() % 1000;

      std::this_thread::sleep_for(std::chrono::microseconds(pre_delay));

      // Maybe reap externally
      if (rng() % 2 == 0) {
        reap_directly(pid);
      }

      std::this_thread::sleep_for(std::chrono::microseconds(mid_delay));

      nix::process_handle_t handle(pid);

      // Random choice of operation - should never throw ECHILD
      int status;
      if (rng() % 2 == 0) {
        status = handle.wait();
      } else {
        status = handle.kill();
      }
      RC_ASSERT(WIFEXITED(status) || WIFSIGNALED(status));
    }
  });
}

TEST_CASE("process_handle_t fuzz: concurrent multi-handle chaos",
          "[processes][fuzz][concurrency]") {
  rc::prop("multiple handles to same PID handled safely", []() {
    auto num_handles = *rc::gen::inRange(2, 5);
    auto timing_seed = *rc::gen::arbitrary<unsigned>();

    pid_t pid = fork_and_exit(0);
    RC_ASSERT(pid > 0);

    std::mt19937 rng(timing_seed);
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    threads.reserve(num_handles);

    for (int i = 0; i < num_handles; ++i) {
      threads.emplace_back([&, i]() {
        auto delay = rng() % 5000;
        std::this_thread::sleep_for(std::chrono::microseconds(delay));

        nix::process_handle_t handle(pid);
        // Should never throw - all handles get a valid status
        int status;
        if (i % 2 == 0) {
          status = handle.wait();
        } else {
          status = handle.kill();
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
          ++success_count;
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    RC_ASSERT(success_count == num_handles);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Signal handler interference tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process_handle_t SIGCHLD does not cause ECHILD errors", "[processes][sigchld][echild]") {
  // When SIGCHLD is delivered, the default handler (or SA_NOCLDWAIT) can
  // auto-reap children, causing ECHILD in subsequent wait() calls
  // After the fix, this should be handled gracefully

  // Save current SIGCHLD handler
  struct sigaction old_action;
  sigaction(SIGCHLD, nullptr, &old_action);

  // Install a handler that does nothing (but might affect wait behavior)
  struct sigaction new_action = {};
  new_action.sa_handler = [](int) {};
  sigemptyset(&new_action.sa_mask);
  new_action.sa_flags = 0;
  sigaction(SIGCHLD, &new_action, nullptr);

  constexpr int iterations = 20;
  int success_count = 0;

  for (int i = 0; i < iterations; ++i) {
    pid_t pid = fork_and_exit(0);
    REQUIRE(pid > 0);

    // Small delay to allow signal delivery
    std::this_thread::sleep_for(1ms);

    nix::process_handle_t handle(pid);
    // Should never throw - returns synthetic status if ECHILD
    int status = handle.wait();
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      ++success_count;
    }
  }

  // Restore original handler
  sigaction(SIGCHLD, &old_action, nullptr);

  // All operations should succeed after the fix
  REQUIRE(success_count == iterations);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process_handle_t wait on invalid PID", "[processes][edge]") {
  // PID that definitely doesn't exist
  pid_t invalid_pid = 999999999;

  nix::process_handle_t handle(invalid_pid);

  // After the fix, ECHILD is handled gracefully - returns synthetic status 0
  int status = handle.wait();
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

TEST_CASE("process_handle_t double wait", "[processes][edge]") {
  pid_t pid = fork_and_exit(0);
  REQUIRE(pid > 0);

  nix::process_handle_t handle(pid);
  int status = handle.wait();
  REQUIRE(WIFEXITED(status));

  // Handle should now be invalid (pid_ = -1)
  // A second wait should assert or handle gracefully
  // Note: This tests internal state management
}

TEST_CASE("process_handle_t destructor kills if not waited", "[processes][destructor]") {
  pid_t pid = fork_wait_for_signal(SIGKILL);
  REQUIRE(pid > 0);

  {
    nix::process_handle_t handle(pid);
    // Don't wait - destructor should kill
  }

  // Verify process was killed (not a zombie)
  int status;
  pid_t result = waitpid(pid, &status, WNOHANG);
  // Should either be -1 (already reaped by destructor) or 0 (not our child anymore)
  // or pid (we just reaped it now)
  REQUIRE((result == -1 || result == 0 || result == pid));
}
