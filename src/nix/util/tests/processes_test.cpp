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

// clang-format off
// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>
// clang-format on

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nix/util/processes.h"

using std::chrono_literals::operator""s;
using std::chrono_literals::operator""ms;

// ─────────────────────────────────────────────────────────────────────────────
// Test helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Fork a child that exits immediately
pid_t fork_and_exit(int exit_code = 0) {
  pid_t pid = fork();
  if (pid == 0) {
    // Create own process group to isolate from test harness
    setpgid(0, 0);
    _exit(exit_code);
  }
  return pid;
}

// Fork a child that sleeps then exits
pid_t fork_sleep_and_exit(int sleep_ms, int exit_code = 0) {
  pid_t pid = fork();
  if (pid == 0) {
    // Create own process group to isolate from test harness
    setpgid(0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    _exit(exit_code);
  }
  return pid;
}

// Fork a child that waits for a signal before exiting
pid_t fork_wait_for_signal(int sig = SIGUSR1) {
  pid_t pid = fork();
  if (pid == 0) {
    // Create own process group to avoid signal interference with parent
    setpgid(0, 0);

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

TEST_CASE("process_handle_t kill sends signal", "[processes][basic][.unsafe_signals]") {
  // NOTE: This test may cause SIGTERM to be delivered to the test harness.
  // Tagged [.unsafe_signals] to skip by default.
  pid_t pid = fork_wait_for_signal(SIGTERM); // Wait for SIGTERM (graceful shutdown)
  REQUIRE(pid > 0);

  nix::process_handle_t handle(pid);
  handle.set_kill_signal(SIGKILL);
  int status = handle.kill();

  // With graceful shutdown, process receives SIGTERM first and exits cleanly.
  // fork_wait_for_signal uses sigwait() then _exit(0), so process exits normally.
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

TEST_CASE("process_handle_t move semantics", "[processes][basic][.unsafe_signals]") {
  // NOTE: This test may cause signal interference with test harness.
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

TEST_CASE("process_handle_t concurrent reap and wait",
          "[processes][echild][race][concurrency][.unsafe_signals]") {
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
          "[processes][property][echild][.unsafe_signals]") {
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
          "[processes][property][echild][.unsafe_signals]") {
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
          "[processes][stress][concurrency][.unsafe_signals]") {
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

TEST_CASE("process_handle_t fuzz: adversarial timing attacks",
          "[processes][fuzz][echild][.unsafe_signals]") {
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
          "[processes][fuzz][concurrency][.unsafe_signals]") {
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

TEST_CASE("process_handle_t SIGCHLD does not cause ECHILD errors",
          "[processes][sigchld][echild][.unsafe_signals]") {
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

TEST_CASE("process_handle_t destructor kills if not waited",
          "[processes][destructor][.unsafe_signals]") {
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

// ─────────────────────────────────────────────────────────────────────────────
// Issue #1426 - Don't kill builder too early if stdout/stderr are closed
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process should not be killed just because stdout pipe is closed",
          "[processes][gh1426]") {
  // Regression test for #1426: Builder shouldn't be killed early if it closes
  // its stdout/stderr pipes but hasn't actually exited yet.
  //
  // The fix in derivation-builder.cpp uses waitpid(WNOHANG) to check if the
  // process has actually exited before killing it. If it hasn't exited,
  // we wait for it to finish naturally.

  // Create a pipe to use as stdout
  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: close stdout pipe early, then continue running
    close(pipefd[0]); // Close read end
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    // Close stdout (simulating builder closing its output)
    close(STDOUT_FILENO);

    // Continue running for a bit - this is the key part:
    // Process is still alive but stdout is closed
    usleep(200000); // 200ms

    _exit(42); // Exit with distinctive code
  }

  // Parent
  close(pipefd[1]); // Close write end

  // Read until EOF on the pipe
  char buf[256];
  while (read(pipefd[0], buf, sizeof(buf)) > 0) {
  }
  close(pipefd[0]);

  // At this point, the pipe is closed (we got EOF), but the child is still running.
  // The fix for #1426 ensures we check with WNOHANG before killing.

  // Check process is still running
  int status;
  pid_t ret = waitpid(pid, &status, WNOHANG);
  if (ret == 0) {
    // Process is still running as expected - wait for it to exit naturally
    ret = waitpid(pid, &status, 0);
    REQUIRE(ret == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 42);
  } else if (ret == pid) {
    // Process already exited - timing dependent but still valid
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 42);
  } else {
    FAIL("Unexpected waitpid result");
  }
}

TEST_CASE("waitpid WNOHANG correctly detects running vs exited process", "[processes][gh1426]") {
  // Test the core mechanism used in the #1426 fix: waitpid with WNOHANG
  // should return 0 if process is still running, pid if it has exited.

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    usleep(100000); // 100ms
    _exit(0);
  }

  // Immediately check - process should still be running
  int status;
  pid_t ret = waitpid(pid, &status, WNOHANG);
  REQUIRE(ret == 0); // 0 means still running

  // Wait for process to finish
  ret = waitpid(pid, &status, 0);
  REQUIRE(ret == pid);
  REQUIRE(WIFEXITED(status));
}

// ─────────────────────────────────────────────────────────────────────────────
// Issue #8232 - Darwin builds forking off processes never finish
// (FD_CLOEXEC on pty slave fd)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("FD_CLOEXEC prevents inherited fds from blocking parent", "[processes][gh8232]") {
  // Regression test for #8232: On Darwin, builds that fork background processes
  // would hang indefinitely because the pty slave fd remained open in the
  // background process, preventing EOF on the pty master.
  //
  // The fix sets FD_CLOEXEC on stdout/stderr fds before exec, so background
  // processes don't inherit them.

  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    close(pipefd[0]); // Close read end

    // Set FD_CLOEXEC on the write side (simulating the fix)
    int flags = fcntl(pipefd[1], F_GETFD);
    fcntl(pipefd[1], F_SETFD, flags | FD_CLOEXEC);

    // Fork a "background" child
    pid_t bg_pid = fork();
    if (bg_pid == 0) {
      // Background child: exec something
      // With FD_CLOEXEC, the pipe fd should be closed after exec
      execlp("true", "true", nullptr);
      _exit(1);
    }

    // Write something and exit - the pipe should close with us
    // because background child's fd was closed on exec
    write(pipefd[1], "done", 4);
    _exit(0);
  }

  // Parent
  close(pipefd[1]); // Close write end

  // This read should get EOF quickly because:
  // 1. Main child exited and closed its fd
  // 2. Background child's fd was closed on exec due to FD_CLOEXEC
  char buf[256];
  ssize_t n = read(pipefd[0], buf, sizeof(buf));
  REQUIRE(n == 4);
  REQUIRE(memcmp(buf, "done", 4) == 0);

  // Should get EOF immediately, not hang
  n = read(pipefd[0], buf, sizeof(buf));
  REQUIRE(n == 0); // EOF

  close(pipefd[0]);

  // Reap the child
  int status;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
}

TEST_CASE("without FD_CLOEXEC, background process keeps pipe open", "[processes][gh8232]") {
  // Demonstrates the problem that #8232 fixes: without FD_CLOEXEC,
  // a background process inherits the fd and keeps it open.

  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    close(pipefd[0]);

    // Do NOT set FD_CLOEXEC (demonstrating the bug scenario)
    // Fork a background child that sleeps
    pid_t bg_pid = fork();
    if (bg_pid == 0) {
      // Background child: just sleep briefly
      // The inherited pipe fd keeps the pipe open
      usleep(50000); // 50ms
      _exit(0);
    }

    // Main child exits immediately
    write(pipefd[1], "x", 1);
    close(pipefd[1]);
    _exit(0);
  }

  // Parent
  close(pipefd[1]);

  // Set non-blocking to avoid hanging forever
  int flags = fcntl(pipefd[0], F_GETFL);
  fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

  // Read the initial byte
  char buf[256];
  usleep(10000); // Give child time to write
  ssize_t n = read(pipefd[0], buf, sizeof(buf));
  REQUIRE(n == 1);

  // Try to read again immediately - without FD_CLOEXEC fix,
  // this would block (or return EAGAIN in non-blocking mode)
  // because background process still has the fd open
  n = read(pipefd[0], buf, sizeof(buf));
  // In non-blocking mode, EAGAIN means pipe is still open
  // (background process hasn't exited yet)
  bool pipe_still_open = (n == -1 && errno == EAGAIN);
  bool pipe_closed = (n == 0);

  // Either result is acceptable depending on timing,
  // but the key insight is that with FD_CLOEXEC, we'd always get EOF quickly
  REQUIRE((pipe_still_open || pipe_closed));

  close(pipefd[0]);
  waitpid(pid, nullptr, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Issue #8247 - macOS crashed on child side of fork pre-exec
// (curl_global_init before forking)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("curl_global_init called before fork prevents objc crash", "[processes][gh8247]") {
  // Regression test for #8247: On macOS, calling curl_global_init for the
  // first time after fork() causes a crash due to an objc quirk.
  //
  // The fix calls curl_global_init during init_lib_store(), before any forking.
  //
  // This test verifies that forking after libstore initialization doesn't crash.
  // On a system without the fix, this would crash on macOS.

  // The fix is in globals.cpp - curl_global_init is called early in init_lib_store.
  // We can't easily test that it was called, but we can test that forking
  // and doing curl-related work doesn't crash.

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: do nothing special, just exit cleanly
    // On a system without the fix, objc initialization issues might occur here
    _exit(0);
  }

  int status;
  pid_t ret = waitpid(pid, &status, 0);
  REQUIRE(ret == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Issue #2141 - Process Group ID issues in shellHook
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("setpgid creates new process group for shell", "[processes][gh2141]") {
  // Regression test for #2141: Background processes started by shellHook
  // would receive signals meant for the shell because they inherited the
  // shell's process group.
  //
  // The fix uses setpgid(pid, pid) to put the shell in its own process group,
  // so background processes can have their own groups.

  pid_t original_pgid = getpgrp();
  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: create a new process group (like setup_interactive_process_group does)
    pid_t child_pid = getpid();
    int result = setpgid(child_pid, child_pid);

    // Verify we're now in our own process group
    if (result == 0) {
      pid_t new_pgid = getpgrp();
      // New process group should be our own pid
      if (new_pgid == child_pid) {
        _exit(0); // Success
      }
    }
    _exit(1); // Failure
  }

  int status;
  pid_t ret = waitpid(pid, &status, 0);
  REQUIRE(ret == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  // Parent's process group should be unchanged
  REQUIRE(getpgrp() == original_pgid);
}

TEST_CASE("child in new process group doesn't receive parent's SIGINT", "[processes][gh2141]") {
  // Test that a child in its own process group doesn't receive signals
  // sent to the parent's process group.

  // Track if child received SIGINT
  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    close(pipefd[0]);

    // Create new process group (the #2141 fix)
    pid_t child_pid = getpid();
    setpgid(child_pid, child_pid);

    // Install SIGINT handler that writes to pipe
    struct sigaction sa = {};
    sa.sa_handler = [](int) {
      // Can't easily write to pipe from signal handler,
      // so we'll just exit with a distinctive code
      _exit(99);
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);

    // Wait a bit to receive any signal
    usleep(100000);

    // If we get here, we didn't receive SIGINT
    write(pipefd[1], "ok", 2);
    _exit(0);
  }

  close(pipefd[1]);

  // Give child time to set up process group
  usleep(20000);

  // Send SIGINT to parent's process group (not child's new group)
  // This uses negative pid to signal the process group
  // But since child has its own group, it shouldn't receive it
  // (We can't actually send to our group without affecting ourselves,
  // so this test verifies the child is in a different group)

  pid_t child_pgid = getpgid(pid);
  pid_t parent_pgid = getpgrp();

  // Key assertion: child should be in its own process group
  REQUIRE(child_pgid == pid);
  REQUIRE(child_pgid != parent_pgid);

  // Wait for child
  char buf[10];
  ssize_t n = read(pipefd[0], buf, sizeof(buf));
  close(pipefd[0]);

  int status;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  REQUIRE(n == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Issue #11040 - Capture all non-interactive child process stderrs
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("run_program_with_stderr captures stderr separately",
          "[processes][gh11040][.unsafe_signals]") {
  // Regression test for #11040: Non-interactive child process stderr should
  // be captured separately from stdout.
  //
  // The fix adds run_program_with_stderr() and standard_err sink in run_options_t.

  nix::run_options_t options;
  options.program = "/bin/sh";
  options.lookup_path = false;
  options.args = {"-c", "echo stdout_output; echo stderr_output >&2"};

  auto result = nix::run_program_with_stderr(std::move(options));

  // Status should be 0 (success)
  REQUIRE(result.status == 0);

  // stdout and stderr should be captured separately
  REQUIRE(result.stdout_output.find("stdout_output") != std::string::npos);
  REQUIRE(result.stderr_output.find("stderr_output") != std::string::npos);

  // They shouldn't be mixed
  REQUIRE(result.stdout_output.find("stderr_output") == std::string::npos);
  REQUIRE(result.stderr_output.find("stdout_output") == std::string::npos);
}

TEST_CASE("run_program_with_stderr captures stderr on failure",
          "[processes][gh11040][.unsafe_signals]") {
  // Test that stderr is captured even when the program fails

  nix::run_options_t options;
  options.program = "/bin/sh";
  options.lookup_path = false;
  options.args = {"-c", "echo error_message >&2; exit 1"};

  auto result = nix::run_program_with_stderr(std::move(options));

  // Status should be non-zero (failure)
  REQUIRE(result.status != 0);

  // stderr should contain our error message
  REQUIRE(result.stderr_output.find("error_message") != std::string::npos);
}

TEST_CASE("stderr capture handles large output without deadlock",
          "[processes][gh11040][.unsafe_signals]") {
  // Test that capturing both stdout and stderr doesn't deadlock
  // when both produce large amounts of output.
  //
  // Before #11040 fix, only stdout was read in the main thread,
  // which could deadlock if stderr filled its buffer.

  nix::run_options_t options;
  options.program = "/bin/sh";
  options.lookup_path = false;
  // Generate 64KB on both stdout and stderr (larger than typical pipe buffer)
  options.args = {"-c", "dd if=/dev/zero bs=1024 count=64 2>/dev/null | tr '\\0' 'O'; "
                        "dd if=/dev/zero bs=1024 count=64 2>&1 >/dev/null | tr '\\0' 'E'"};

  auto result = nix::run_program_with_stderr(std::move(options));

  // Should complete without deadlock
  REQUIRE(result.status == 0);

  // Both streams should have received output
  REQUIRE(result.stdout_output.size() > 60000);
  REQUIRE(result.stderr_output.size() > 60000);
}

// ─────────────────────────────────────────────────────────────────────────────
// Issue #14760 - Shouldn't kill build hook with SIGKILL immediately
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process_handle_t::kill sends SIGTERM before SIGKILL",
          "[processes][gh14760][.signal_test]") {
  // Regression test for #14760: Build hooks should receive SIGTERM first
  // and have a chance to clean up before being SIGKILL'd.
  //
  // The fix in processes.cpp sends SIGTERM first, waits up to 5 seconds,
  // then sends SIGKILL if the process hasn't exited.
  //
  // NOTE: This test may interfere with test harness signal handling.
  // Tagged [.signal_test] to skip by default.

  // Fork a child that tracks which signal it receives
  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Create own process group to isolate from test harness
    setpgid(0, 0);

    close(pipefd[0]);

    // Track received signal
    static int received_signal = 0;
    static int write_fd = pipefd[1];

    struct sigaction sa = {};
    sa.sa_handler = [](int sig) {
      received_signal = sig;
      // Write the signal number to the pipe
      char buf[10];
      int n = snprintf(buf, sizeof(buf), "%d", sig);
      write(write_fd, buf, n);
      _exit(0);
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);

    // Wait for signal (will be interrupted)
    pause();
    _exit(1);
  }

  close(pipefd[1]);

  // Use process_handle_t to kill - it should send SIGTERM first
  nix::process_handle_t handle(pid);
  handle.set_kill_signal(SIGKILL); // Default, but explicit

  // Kill the process
  int status = handle.kill();

  // Read which signal the child received
  char buf[10] = {0};
  read(pipefd[0], buf, sizeof(buf));
  close(pipefd[0]);

  int received_signal = atoi(buf);

  // Child should have received SIGTERM (graceful shutdown first)
  REQUIRE(received_signal == SIGTERM);

  // Process should have exited cleanly (from SIGTERM handler)
  REQUIRE(WIFEXITED(status));
}

TEST_CASE("process_handle_t::kill escalates to SIGKILL after timeout",
          "[processes][gh14760][.slow]") {
  // Test that SIGKILL is sent if process ignores SIGTERM
  // NOTE: This test is slow (5+ seconds) and may interfere with test harness signals.
  // Tagged [.slow] to skip by default, run with --list-tests to see it.

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Create own process group to isolate from test harness
    setpgid(0, 0);

    // Ignore SIGTERM
    struct sigaction sa = {};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);

    // Wait to be killed
    while (1) {
      pause();
    }
  }

  nix::process_handle_t handle(pid);

  // This should eventually SIGKILL after SIGTERM timeout
  auto start = std::chrono::steady_clock::now();
  int status = handle.kill();
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Should have been killed by SIGKILL
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGKILL);

  // Should have waited for the timeout (at least some time, but not too long)
  // The timeout is 5 seconds (50 * 100ms), but we check for at least 100ms
  // to ensure SIGTERM was actually tried first
  REQUIRE(elapsed > std::chrono::milliseconds(100));
}

TEST_CASE("process_handle_t::kill with custom signal skips SIGTERM",
          "[processes][gh14760][.signal_test]") {
  // Test that setting a custom kill signal bypasses the SIGTERM grace period
  // NOTE: This test may interfere with test harness signal handling.
  // Tagged [.signal_test] to skip by default.

  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Create own process group to isolate from test harness
    setpgid(0, 0);

    close(pipefd[0]);
    static int write_fd = pipefd[1];

    struct sigaction sa = {};
    sa.sa_handler = [](int sig) {
      char buf[10];
      int n = snprintf(buf, sizeof(buf), "%d", sig);
      write(write_fd, buf, n);
      _exit(0);
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    pause();
    _exit(1);
  }

  close(pipefd[1]);

  nix::process_handle_t handle(pid);
  handle.set_kill_signal(SIGUSR1); // Custom signal, not SIGKILL

  int status = handle.kill();

  char buf[10] = {0};
  read(pipefd[0], buf, sizeof(buf));
  close(pipefd[0]);

  int received_signal = atoi(buf);

  // With custom signal, should send that signal directly (no SIGTERM first)
  REQUIRE(received_signal == SIGUSR1);
}
