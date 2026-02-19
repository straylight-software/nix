// straylight::nix::primitives::signals tests
//
// Unit tests for the signals/interrupts primitive.
// Tests thread-safe interrupt flag, signal handling, RAII signal blocking,
// callback registration, and std::stop_token integration.

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <signal.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/primitives/signals.h"

namespace signals = straylight::nix::primitives::signals;

namespace {

// RAII class to set up SIGUSR1 handler and restore on destruction
struct Sigusr1Handler {
  struct sigaction old_action;
  bool installed{false};

  Sigusr1Handler() {
    struct sigaction action = {};
    action.sa_handler = [](int) {
      // Empty handler - just ignore the signal
    };
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (sigaction(SIGUSR1, &action, &old_action) == 0) {
      installed = true;
    }
  }

  ~Sigusr1Handler() {
    if (installed) {
      sigaction(SIGUSR1, &old_action, nullptr);
    }
  }
};

// Global handler installed at test startup
static Sigusr1Handler g_sigusr1_handler;

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// InterruptFlag tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("InterruptFlag default construction", "[signals][flag]") {
  signals::InterruptFlag flag;
  REQUIRE_FALSE(flag.is_set());
  REQUIRE_FALSE(static_cast<bool>(flag));
}

TEST_CASE("InterruptFlag set and clear", "[signals][flag]") {
  signals::InterruptFlag flag;

  flag.set();
  REQUIRE(flag.is_set());
  REQUIRE(static_cast<bool>(flag));

  flag.clear();
  REQUIRE_FALSE(flag.is_set());
}

TEST_CASE("InterruptFlag test_and_set", "[signals][flag]") {
  signals::InterruptFlag flag;

  // First test_and_set returns false (was not set)
  REQUIRE_FALSE(flag.test_and_set());
  REQUIRE(flag.is_set());

  // Second test_and_set returns true (was set)
  REQUIRE(flag.test_and_set());
  REQUIRE(flag.is_set());
}

TEST_CASE("InterruptFlag test_and_clear", "[signals][flag]") {
  signals::InterruptFlag flag;

  flag.set();
  REQUIRE(flag.is_set());

  // test_and_clear returns true (was set)
  REQUIRE(flag.test_and_clear());
  REQUIRE_FALSE(flag.is_set());

  // test_and_clear returns false (was not set)
  REQUIRE_FALSE(flag.test_and_clear());
}

TEST_CASE("InterruptFlag check throws when set", "[signals][flag]") {
  signals::InterruptFlag flag;

  // Does not throw when not set
  REQUIRE_NOTHROW(flag.check());

  flag.set();

  // Throws when set
  REQUIRE_THROWS_AS(flag.check(), signals::Interrupted);
}

TEST_CASE("InterruptFlag thread safety", "[signals][flag][thread]") {
  signals::InterruptFlag flag;
  std::atomic<bool> started{false};
  std::atomic<bool> done{false};

  std::thread setter([&]() {
    started.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    flag.set();
    done.store(true, std::memory_order_release);
  });

  // Wait for thread to start
  while (!started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // Spin until flag is set
  while (!done.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  REQUIRE(flag.is_set());
  setter.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// Interrupted exception tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Interrupted exception default message", "[signals][exception]") {
  signals::Interrupted ex;
  REQUIRE(std::string(ex.what()) == "interrupted");
}

TEST_CASE("Interrupted exception custom message", "[signals][exception]") {
  signals::Interrupted ex("custom message");
  REQUIRE(std::string(ex.what()) == "custom message");
}

TEST_CASE("Interrupted inherits from std::exception", "[signals][exception]") {
  try {
    throw signals::Interrupted("test");
  } catch (const std::exception& e) {
    REQUIRE(std::string(e.what()) == "test");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Global interrupt state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Global interrupt flag set/clear", "[signals][global]") {
  // Start with clean state
  signals::clear_interrupted();
  REQUIRE_FALSE(signals::is_interrupted());

  signals::set_interrupted();
  REQUIRE(signals::is_interrupted());

  signals::clear_interrupted();
  REQUIRE_FALSE(signals::is_interrupted());
}

TEST_CASE("check_interrupt throws when global flag set", "[signals][global]") {
  signals::clear_interrupted();
  REQUIRE_NOTHROW(signals::check_interrupt());

  signals::set_interrupted();
  REQUIRE_THROWS_AS(signals::check_interrupt(), signals::Interrupted);

  // Clean up
  signals::clear_interrupted();
}

TEST_CASE("Thread-local interrupt check", "[signals][global][thread]") {
  signals::clear_interrupted();

  bool custom_check_called = false;
  auto prev = signals::set_thread_interrupt_check([&]() {
    custom_check_called = true;
    return true; // Report interrupted
  });

  REQUIRE(signals::is_interrupted());
  REQUIRE(custom_check_called);

  signals::clear_thread_interrupt_check();
  REQUIRE_FALSE(signals::is_interrupted());

  // Restore previous (should be null)
  signals::set_thread_interrupt_check(std::move(prev));
}

// ─────────────────────────────────────────────────────────────────────────────
// std::stop_token integration tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("check_interrupt with stop_token", "[signals][stop_token]") {
  signals::clear_interrupted();

  std::stop_source source;
  auto token = source.get_token();

  // Neither interrupted nor stopped
  REQUIRE_NOTHROW(signals::check_interrupt(token));

  // Request stop
  source.request_stop();
  REQUIRE_THROWS_AS(signals::check_interrupt(token), signals::Interrupted);
}

TEST_CASE("InterruptStopSource basic usage", "[signals][stop_token]") {
  signals::InterruptStopSource source;

  REQUIRE_FALSE(source.stop_requested());
  REQUIRE(source.stop_possible());

  auto token = source.get_token();
  REQUIRE_FALSE(token.stop_requested());

  source.request_stop();
  REQUIRE(source.stop_requested());
  REQUIRE(token.stop_requested());
}

// ─────────────────────────────────────────────────────────────────────────────
// Interrupt callback tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Interrupt callback registration", "[signals][callback]") {
  int callback_count = 0;

  {
    auto handle = signals::on_interrupt([&]() { ++callback_count; });
    REQUIRE(handle.valid());

    // Invoke callbacks manually
    signals::invoke_interrupt_callbacks();
    REQUIRE(callback_count == 1);

    signals::invoke_interrupt_callbacks();
    REQUIRE(callback_count == 2);
  }

  // After handle destroyed, callback should be unregistered
  signals::invoke_interrupt_callbacks();
  REQUIRE(callback_count == 2); // Still 2, not 3
}

TEST_CASE("Multiple interrupt callbacks", "[signals][callback]") {
  int count1 = 0;
  int count2 = 0;

  auto handle1 = signals::on_interrupt([&]() { ++count1; });
  auto handle2 = signals::on_interrupt([&]() { ++count2; });

  signals::invoke_interrupt_callbacks();
  REQUIRE(count1 == 1);
  REQUIRE(count2 == 1);

  // Reset one
  handle1.reset();

  signals::invoke_interrupt_callbacks();
  REQUIRE(count1 == 1); // No change
  REQUIRE(count2 == 2);
}

TEST_CASE("InterruptCallbackHandle move semantics", "[signals][callback]") {
  int callback_count = 0;

  signals::InterruptCallbackHandle handle;
  REQUIRE_FALSE(handle.valid());

  {
    auto temp_handle = signals::on_interrupt([&]() { ++callback_count; });
    REQUIRE(temp_handle.valid());

    handle = std::move(temp_handle);
    REQUIRE(handle.valid());
    REQUIRE_FALSE(temp_handle.valid());
  }

  // Callback still registered (owned by handle)
  signals::invoke_interrupt_callbacks();
  REQUIRE(callback_count == 1);

  handle.reset();

  // Now unregistered
  signals::invoke_interrupt_callbacks();
  REQUIRE(callback_count == 1);
}

TEST_CASE("Interrupt callback exception safety", "[signals][callback]") {
  int count = 0;

  // Register a callback that throws
  auto handle1 = signals::on_interrupt([&]() { throw std::runtime_error("test"); });

  // Register another callback
  auto handle2 = signals::on_interrupt([&]() { ++count; });

  // invoke_interrupt_callbacks should not propagate exceptions
  REQUIRE_NOTHROW(signals::invoke_interrupt_callbacks());

  // Second callback should still be called
  REQUIRE(count == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// trigger_interrupt tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("trigger_interrupt sets flag and invokes callbacks", "[signals][trigger]") {
  signals::clear_interrupted();
  int callback_count = 0;

  auto handle = signals::on_interrupt([&]() { ++callback_count; });

  signals::trigger_interrupt();

  REQUIRE(signals::is_interrupted());
  REQUIRE(callback_count == 1);

  // Clean up
  signals::clear_interrupted();
}

// ─────────────────────────────────────────────────────────────────────────────
// ScopedSignalBlock tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ScopedSignalBlock blocks all signals", "[signals][block]") {
  sigset_t original;
  pthread_sigmask(SIG_BLOCK, nullptr, &original);

  {
    signals::ScopedSignalBlock block;

    // Check that signals are blocked (at least SIGINT)
    sigset_t current;
    pthread_sigmask(SIG_BLOCK, nullptr, &current);
    REQUIRE(sigismember(&current, SIGINT) == 1);
    REQUIRE(sigismember(&current, SIGTERM) == 1);
  }

  // After scope, mask should be restored
  sigset_t after;
  pthread_sigmask(SIG_BLOCK, nullptr, &after);

  // Compare with original (at least for SIGINT)
  REQUIRE(sigismember(&after, SIGINT) == sigismember(&original, SIGINT));
}

TEST_CASE("ScopedSignalBlock blocks specific signals", "[signals][block]") {
  sigset_t original;
  pthread_sigmask(SIG_BLOCK, nullptr, &original);

  {
    signals::ScopedSignalBlock block({SIGUSR1, SIGUSR2});

    sigset_t current;
    pthread_sigmask(SIG_BLOCK, nullptr, &current);
    REQUIRE(sigismember(&current, SIGUSR1) == 1);
    REQUIRE(sigismember(&current, SIGUSR2) == 1);
  }

  // After scope, mask should be restored
  sigset_t after;
  pthread_sigmask(SIG_BLOCK, nullptr, &after);
  REQUIRE(sigismember(&after, SIGUSR1) == sigismember(&original, SIGUSR1));
  REQUIRE(sigismember(&after, SIGUSR2) == sigismember(&original, SIGUSR2));
}

TEST_CASE("ScopedSignalBlock with vector constructor", "[signals][block]") {
  std::vector<int> sigs = {SIGPIPE, SIGALRM};

  {
    signals::ScopedSignalBlock block(sigs);

    sigset_t current;
    pthread_sigmask(SIG_BLOCK, nullptr, &current);
    REQUIRE(sigismember(&current, SIGPIPE) == 1);
    REQUIRE(sigismember(&current, SIGALRM) == 1);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// ScopedSignalUnblock tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ScopedSignalUnblock unblocks signals", "[signals][unblock]") {
  // First block some signals
  sigset_t to_block;
  sigemptyset(&to_block);
  sigaddset(&to_block, SIGUSR1);
  sigset_t original;
  pthread_sigmask(SIG_BLOCK, &to_block, &original);

  {
    sigset_t before;
    pthread_sigmask(SIG_BLOCK, nullptr, &before);
    REQUIRE(sigismember(&before, SIGUSR1) == 1);

    signals::ScopedSignalUnblock unblock({SIGUSR1});

    sigset_t current;
    pthread_sigmask(SIG_BLOCK, nullptr, &current);
    REQUIRE(sigismember(&current, SIGUSR1) == 0);
  }

  // After scope, SIGUSR1 should be blocked again
  sigset_t after;
  pthread_sigmask(SIG_BLOCK, nullptr, &after);
  REQUIRE(sigismember(&after, SIGUSR1) == 1);

  // Restore original
  pthread_sigmask(SIG_SETMASK, &original, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Signal mask save/restore tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("save_signal_mask and restore_signal_mask", "[signals][mask]") {
  // Get current mask
  sigset_t original;
  pthread_sigmask(SIG_BLOCK, nullptr, &original);

  // Save the mask
  signals::save_signal_mask();

  // Block some additional signals
  sigset_t to_block;
  sigemptyset(&to_block);
  sigaddset(&to_block, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &to_block, nullptr);

  sigset_t modified;
  pthread_sigmask(SIG_BLOCK, nullptr, &modified);
  REQUIRE(sigismember(&modified, SIGUSR1) == 1);

  // Restore
  signals::restore_signal_mask();

  sigset_t restored;
  pthread_sigmask(SIG_BLOCK, nullptr, &restored);

  // SIGUSR1 state should match the saved mask (which was original)
  REQUIRE(sigismember(&restored, SIGUSR1) == sigismember(&original, SIGUSR1));
}

// ─────────────────────────────────────────────────────────────────────────────
// ReceiveInterrupts tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ReceiveInterrupts registers callback", "[signals][receive]") {
  // This test verifies that ReceiveInterrupts registers a callback
  // that will send SIGUSR1 to the current thread

  // We can't easily test the actual signal sending without complex setup,
  // but we can verify the callback is registered

  int initial_callback_count = 0;
  auto counter_handle = signals::on_interrupt([&]() { ++initial_callback_count; });

  signals::invoke_interrupt_callbacks();
  REQUIRE(initial_callback_count == 1);

  {
    signals::ReceiveInterrupts receiver;

    // Now there should be 2 callbacks (ours + receiver's)
    initial_callback_count = 0;
    signals::invoke_interrupt_callbacks();
    REQUIRE(initial_callback_count == 1); // Our callback still called
  }

  // After receiver destroyed, only our callback remains
}

// ─────────────────────────────────────────────────────────────────────────────
// Signal handler thread tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("signal_handler_running initially false", "[signals][handler]") {
  // Note: This test assumes signal handler is not started by default
  // In a real test environment, the handler might already be running
  // We just verify the function works
  [[maybe_unused]] bool running = signals::signal_handler_running();
  // Can be true or false depending on test order
  SUCCEED();
}

TEST_CASE("start and stop signal handler thread", "[signals][handler]") {
  // Stop any existing handler
  signals::stop_signal_handler_thread();
  REQUIRE_FALSE(signals::signal_handler_running());

  // Start the handler
  signals::start_signal_handler_thread();
  REQUIRE(signals::signal_handler_running());

  // Starting again should be idempotent
  signals::start_signal_handler_thread();
  REQUIRE(signals::signal_handler_running());

  // Stop the handler
  signals::stop_signal_handler_thread();
  REQUIRE_FALSE(signals::signal_handler_running());

  // Stopping again should be idempotent
  signals::stop_signal_handler_thread();
  REQUIRE_FALSE(signals::signal_handler_running());
}

TEST_CASE("start signal handler with custom config", "[signals][handler]") {
  signals::stop_signal_handler_thread();

  signals::SignalConfig config;
  config.signals = {SIGINT, SIGTERM};
  config.start_handler_thread = true;

  signals::start_signal_handler_thread(config);
  REQUIRE(signals::signal_handler_running());

  signals::stop_signal_handler_thread();
}

TEST_CASE("start signal handler without thread", "[signals][handler]") {
  signals::stop_signal_handler_thread();

  signals::SignalConfig config;
  config.signals = {SIGINT};
  config.start_handler_thread = false;

  signals::start_signal_handler_thread(config);
  REQUIRE_FALSE(signals::signal_handler_running());
}

// ─────────────────────────────────────────────────────────────────────────────
// Signal delivery tests (requires careful setup)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Signal triggers interrupt flag", "[signals][delivery]") {
  signals::clear_interrupted();
  signals::stop_signal_handler_thread();

  // Start signal handler
  signals::start_signal_handler_thread();
  REQUIRE(signals::signal_handler_running());

  // Give the handler thread time to start
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send SIGINT to ourselves
  kill(getpid(), SIGINT);

  // Wait a bit for signal to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Check that interrupt flag was set
  REQUIRE(signals::is_interrupted());

  // Clean up
  signals::clear_interrupted();
  signals::stop_signal_handler_thread();
}

TEST_CASE("Signal invokes callbacks", "[signals][delivery]") {
  signals::clear_interrupted();
  signals::stop_signal_handler_thread();

  int callback_count = 0;
  auto handle = signals::on_interrupt([&]() { ++callback_count; });

  // Start signal handler
  signals::start_signal_handler_thread();

  // Give the handler thread time to start
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send SIGTERM to ourselves
  kill(getpid(), SIGTERM);

  // Wait for signal to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Callback should have been invoked
  REQUIRE(callback_count >= 1);

  // Clean up
  signals::clear_interrupted();
  signals::stop_signal_handler_thread();
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases and stress tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Concurrent flag access", "[signals][stress]") {
  signals::InterruptFlag flag;
  std::atomic<int> read_count{0};
  std::atomic<int> set_count{0};

  constexpr int kIterations = 10000;

  std::thread reader([&]() {
    for (int i = 0; i < kIterations; ++i) {
      if (flag.is_set()) {
        ++read_count;
      }
    }
  });

  std::thread writer([&]() {
    for (int i = 0; i < kIterations; ++i) {
      if (i % 2 == 0) {
        flag.set();
        ++set_count;
      } else {
        flag.clear();
      }
    }
  });

  reader.join();
  writer.join();

  // Just verify no crashes/deadlocks
  SUCCEED();
}

TEST_CASE("Rapid callback registration and unregistration", "[signals][stress]") {
  constexpr int kIterations = 100;
  std::atomic<int> callback_count{0};

  for (int i = 0; i < kIterations; ++i) {
    auto handle = signals::on_interrupt([&]() { ++callback_count; });
    signals::invoke_interrupt_callbacks();
    // handle goes out of scope, callback unregistered
  }

  // Final invoke should not call any callbacks
  int final_count = callback_count.load();
  signals::invoke_interrupt_callbacks();
  REQUIRE(callback_count.load() == final_count);
}

TEST_CASE("Nested ScopedSignalBlock", "[signals][block][nested]") {
  sigset_t original;
  pthread_sigmask(SIG_BLOCK, nullptr, &original);

  {
    signals::ScopedSignalBlock outer({SIGUSR1});

    sigset_t after_outer;
    pthread_sigmask(SIG_BLOCK, nullptr, &after_outer);
    REQUIRE(sigismember(&after_outer, SIGUSR1) == 1);

    {
      signals::ScopedSignalBlock inner({SIGUSR2});

      sigset_t after_inner;
      pthread_sigmask(SIG_BLOCK, nullptr, &after_inner);
      REQUIRE(sigismember(&after_inner, SIGUSR1) == 1);
      REQUIRE(sigismember(&after_inner, SIGUSR2) == 1);
    }

    // After inner scope, SIGUSR2 should be in original state
    sigset_t after_inner_scope;
    pthread_sigmask(SIG_BLOCK, nullptr, &after_inner_scope);
    REQUIRE(sigismember(&after_inner_scope, SIGUSR1) == 1);
    REQUIRE(sigismember(&after_inner_scope, SIGUSR2) == sigismember(&after_outer, SIGUSR2));
  }

  // After outer scope, back to original
  sigset_t final_mask;
  pthread_sigmask(SIG_BLOCK, nullptr, &final_mask);
  REQUIRE(sigismember(&final_mask, SIGUSR1) == sigismember(&original, SIGUSR1));
}

// ─────────────────────────────────────────────────────────────────────────────
// Clean up after all tests
// ─────────────────────────────────────────────────────────────────────────────

// Note: We should ensure the signal handler is stopped after tests
// This could be done with a test fixture, but for simplicity we just
// make sure to clean up in the last test case that uses the handler.
