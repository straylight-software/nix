// straylight::nix::sync
//
// Modern C++23 signals/interrupts primitive for cooperative cancellation.
//
// Features:
// - Thread-safe InterruptFlag using std::atomic<bool>
// - Integration with C++20 std::stop_token/std::stop_source
// - Signal handler registration (SIGINT, SIGTERM, etc.)
// - Scoped signal blocking (RAII)
// - Async-signal-safe operations
// - Callback registration for interrupt handling
//
// Usage:
//   signals::check_interrupt();  // Throws Interrupted if interrupted
//
//   auto cb = signals::on_interrupt([] { cleanup(); });  // Register callback
//
//   {
//     signals::ScopedSignalBlock block;  // Block signals in scope
//     // ... critical section ...
//   }
//
//   // Integration with std::jthread:
//   std::jthread worker([](std::stop_token token) {
//     while (!token.stop_requested()) {
//       signals::check_interrupt(token);  // Check both flag and token
//     }
//   });

#pragma once

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// POSIX headers for signal handling
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace straylight::nix::sync {

// ─────────────────────────────────────────────────────────────────────────────
// Exception types
// ─────────────────────────────────────────────────────────────────────────────

/// Exception thrown when an operation is interrupted.
/// Inherits from std::exception for clean integration with standard exception handling.
class Interrupted : public std::exception {
public:
  Interrupted() = default;
  explicit Interrupted(std::string message) : message_(std::move(message)) {}

  [[nodiscard]] const char* what() const noexcept override {
    return message_.empty() ? "interrupted" : message_.c_str();
  }

private:
  std::string message_;
};

// ─────────────────────────────────────────────────────────────────────────────
// InterruptFlag - thread-safe interrupt flag
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe interrupt flag using std::atomic.
/// Can be checked and set from any thread, including signal handlers.
class InterruptFlag {
public:
  InterruptFlag() noexcept = default;

  // Non-copyable, non-movable (atomic semantics)
  InterruptFlag(const InterruptFlag&) = delete;
  InterruptFlag& operator=(const InterruptFlag&) = delete;
  InterruptFlag(InterruptFlag&&) = delete;
  InterruptFlag& operator=(InterruptFlag&&) = delete;

  /// Check if interrupted.
  [[nodiscard]] bool is_set() const noexcept { return flag_.load(std::memory_order_acquire); }

  /// Explicit conversion to bool for use in conditions.
  [[nodiscard]] explicit operator bool() const noexcept { return is_set(); }

  /// Set the interrupt flag.
  /// Async-signal-safe when using atomic operations.
  void set() noexcept { flag_.store(true, std::memory_order_release); }

  /// Clear the interrupt flag.
  void clear() noexcept { flag_.store(false, std::memory_order_release); }

  /// Set the flag and return previous value (atomic exchange).
  [[nodiscard]] bool test_and_set() noexcept {
    return flag_.exchange(true, std::memory_order_acq_rel);
  }

  /// Clear the flag and return previous value (atomic exchange).
  [[nodiscard]] bool test_and_clear() noexcept {
    return flag_.exchange(false, std::memory_order_acq_rel);
  }

  /// Throw Interrupted if the flag is set.
  void check() const {
    if (is_set()) {
      throw Interrupted{};
    }
  }

private:
  std::atomic<bool> flag_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Global interrupt state
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Global interrupt flag - async-signal-safe access.
inline std::atomic<bool> g_interrupted{false};

/// Thread-local additional interrupt check function.
/// Can be used to integrate with custom cancellation mechanisms.
inline thread_local std::function<bool()> tl_interrupt_check;

/// Forward declaration for callback management.
struct InterruptCallbackRegistry;

/// Get the singleton callback registry (lazily initialized).
InterruptCallbackRegistry& get_callback_registry();

/// Saved signal mask for restoration before spawning children.
inline sigset_t g_saved_signal_mask;
inline std::atomic<bool> g_signal_mask_saved{false};

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Core interrupt checking functions
// ─────────────────────────────────────────────────────────────────────────────

/// Set the global interrupt flag.
inline void set_interrupted(bool interrupted = true) noexcept {
  detail::g_interrupted.store(interrupted, std::memory_order_release);
}

/// Clear the global interrupt flag.
inline void clear_interrupted() noexcept {
  detail::g_interrupted.store(false, std::memory_order_release);
}

/// Check if the global interrupt flag is set.
[[nodiscard]] inline bool is_interrupted() noexcept {
  // Fast path: check atomic flag
  if (detail::g_interrupted.load(std::memory_order_acquire)) {
    return true;
  }
  // Slow path: check thread-local custom function
  if (detail::tl_interrupt_check) {
    return detail::tl_interrupt_check();
  }
  return false;
}

/// Throw Interrupted if the global flag is set or thread-local check returns true.
/// Call this in long-running loops and between slow operations.
inline void check_interrupt() {
  if (is_interrupted()) {
    throw Interrupted{};
  }
}

/// Check interrupt with a std::stop_token.
/// Throws Interrupted if either the global flag is set or the token is stopped.
inline void check_interrupt(std::stop_token token) {
  if (is_interrupted() || token.stop_requested()) {
    throw Interrupted{};
  }
}

/// Set a thread-local interrupt check function.
/// Returns the previous check function (if any).
inline std::function<bool()> set_thread_interrupt_check(std::function<bool()> check) {
  auto prev = std::move(detail::tl_interrupt_check);
  detail::tl_interrupt_check = std::move(check);
  return prev;
}

/// Clear the thread-local interrupt check function.
inline void clear_thread_interrupt_check() {
  detail::tl_interrupt_check = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// std::stop_token integration
// ─────────────────────────────────────────────────────────────────────────────

/// Create a std::stop_source that triggers when the global interrupt flag is set.
/// This allows using std::jthread with the global interrupt mechanism.
class InterruptStopSource {
public:
  InterruptStopSource() = default;

  /// Get a stop token for this source.
  [[nodiscard]] std::stop_token get_token() const noexcept { return source_.get_token(); }

  /// Request stop (called when interrupt occurs).
  void request_stop() noexcept { source_.request_stop(); }

  /// Check if stop has been requested.
  [[nodiscard]] bool stop_requested() const noexcept { return source_.stop_requested(); }

  /// Check if stop is possible (always true for this implementation).
  [[nodiscard]] bool stop_possible() const noexcept { return source_.stop_possible(); }

private:
  std::stop_source source_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Interrupt callbacks
// ─────────────────────────────────────────────────────────────────────────────

/// Handle for a registered interrupt callback.
/// The callback is automatically unregistered when the handle is destroyed.
class InterruptCallbackHandle {
public:
  InterruptCallbackHandle() noexcept = default;
  ~InterruptCallbackHandle();

  // Move-only
  InterruptCallbackHandle(InterruptCallbackHandle&& other) noexcept;
  InterruptCallbackHandle& operator=(InterruptCallbackHandle&& other) noexcept;
  InterruptCallbackHandle(const InterruptCallbackHandle&) = delete;
  InterruptCallbackHandle& operator=(const InterruptCallbackHandle&) = delete;

  /// Check if this handle is valid (has a registered callback).
  [[nodiscard]] bool valid() const noexcept { return id_ != 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

  /// Unregister the callback.
  void reset();

private:
  friend InterruptCallbackHandle on_interrupt(std::function<void()>);
  explicit InterruptCallbackHandle(std::uint64_t id) noexcept : id_(id) {}

  std::uint64_t id_{0};
};

/// Register a callback to be invoked when an interrupt occurs.
/// The callback is invoked from a non-signal context (safe to do arbitrary work).
/// Returns a handle that unregisters the callback when destroyed.
[[nodiscard]] InterruptCallbackHandle on_interrupt(std::function<void()> callback);

/// Invoke all registered interrupt callbacks.
/// Called by the signal handler thread when an interrupt is received.
/// Not async-signal-safe - must be called from a regular thread context.
void invoke_interrupt_callbacks();

// ─────────────────────────────────────────────────────────────────────────────
// Receive interrupts in current thread (SIGUSR1 multiplexing)
// ─────────────────────────────────────────────────────────────────────────────

/// RAII class that causes the current thread to receive SIGUSR1 when
/// an interrupt occurs. This allows multiplexing SIGINT to multiple threads.
class ReceiveInterrupts {
public:
  ReceiveInterrupts();
  ~ReceiveInterrupts() = default;

  // Non-copyable, non-movable (registered with specific thread)
  ReceiveInterrupts(const ReceiveInterrupts&) = delete;
  ReceiveInterrupts& operator=(const ReceiveInterrupts&) = delete;
  ReceiveInterrupts(ReceiveInterrupts&&) = delete;
  ReceiveInterrupts& operator=(ReceiveInterrupts&&) = delete;

private:
  pthread_t target_;
  InterruptCallbackHandle callback_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Signal handler management
// ─────────────────────────────────────────────────────────────────────────────

/// Configuration for signal handler setup.
struct SignalConfig {
  /// Signals to handle (default: SIGINT, SIGTERM, SIGHUP)
  std::vector<int> signals = {SIGINT, SIGTERM, SIGHUP};

  /// Whether to start the signal handler thread.
  bool start_handler_thread = true;
};

/// Start the signal handler thread.
/// Blocks the specified signals on the calling thread (and child threads).
/// Saves the signal mask for restoration before spawning child processes.
void start_signal_handler_thread(SignalConfig config = {});

/// Stop the signal handler thread.
/// Should be called before process exit for clean shutdown.
void stop_signal_handler_thread();

/// Check if the signal handler thread is running.
[[nodiscard]] bool signal_handler_running() noexcept;

/// Save the current signal mask.
/// Call this before creating threads if not using start_signal_handler_thread.
void save_signal_mask();

/// Restore the saved signal mask.
/// Call this before fork/exec to ensure child processes get the original mask.
void restore_signal_mask();

/// Trigger an interrupt programmatically.
/// Sets the global interrupt flag and invokes callbacks.
/// Useful for testing or for propagating interrupts from other sources.
void trigger_interrupt();

// ─────────────────────────────────────────────────────────────────────────────
// Signal blocking RAII
// ─────────────────────────────────────────────────────────────────────────────

/// RAII class to block signals in the current thread.
/// Restores the previous signal mask when destroyed.
class ScopedSignalBlock {
public:
  /// Block all signals.
  ScopedSignalBlock();

  /// Block specific signals.
  explicit ScopedSignalBlock(std::initializer_list<int> signals);
  explicit ScopedSignalBlock(const std::vector<int>& signals);

  ~ScopedSignalBlock();

  // Non-copyable, non-movable
  ScopedSignalBlock(const ScopedSignalBlock&) = delete;
  ScopedSignalBlock& operator=(const ScopedSignalBlock&) = delete;
  ScopedSignalBlock(ScopedSignalBlock&&) = delete;
  ScopedSignalBlock& operator=(ScopedSignalBlock&&) = delete;

  /// Get the signals that were blocked.
  [[nodiscard]] const sigset_t& blocked_set() const noexcept { return blocked_; }

private:
  sigset_t previous_;
  sigset_t blocked_;
};

/// RAII class to temporarily unblock signals.
/// Restores the previous signal mask when destroyed.
class ScopedSignalUnblock {
public:
  /// Unblock all signals.
  ScopedSignalUnblock();

  /// Unblock specific signals.
  explicit ScopedSignalUnblock(std::initializer_list<int> signals);
  explicit ScopedSignalUnblock(const std::vector<int>& signals);

  ~ScopedSignalUnblock();

  // Non-copyable, non-movable
  ScopedSignalUnblock(const ScopedSignalUnblock&) = delete;
  ScopedSignalUnblock& operator=(const ScopedSignalUnblock&) = delete;
  ScopedSignalUnblock(ScopedSignalUnblock&&) = delete;
  ScopedSignalUnblock& operator=(ScopedSignalUnblock&&) = delete;

private:
  sigset_t previous_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Implementation details
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Callback registry - manages registered interrupt callbacks.
struct InterruptCallbackRegistry {
  std::mutex mutex;
  std::vector<std::pair<std::uint64_t, std::function<void()>>> callbacks;
  std::uint64_t next_id{1};

  std::uint64_t add(std::function<void()> callback) {
    std::lock_guard lock{mutex};
    auto id = next_id++;
    callbacks.emplace_back(id, std::move(callback));
    return id;
  }

  void remove(std::uint64_t id) {
    std::lock_guard lock{mutex};
    std::erase_if(callbacks, [id](const auto& p) { return p.first == id; });
  }

  void invoke_all() {
    // Copy callbacks under lock, then invoke outside lock to avoid deadlocks
    std::vector<std::function<void()>> to_invoke;
    {
      std::lock_guard lock{mutex};
      to_invoke.reserve(callbacks.size());
      for (const auto& [id, cb] : callbacks) {
        to_invoke.push_back(cb);
      }
    }
    for (const auto& cb : to_invoke) {
      try {
        cb();
      } catch (...) {
        // Swallow exceptions from callbacks
      }
    }
  }
};

inline InterruptCallbackRegistry& get_callback_registry() {
  static InterruptCallbackRegistry registry;
  return registry;
}

/// Signal handler thread state.
inline std::atomic<bool> g_handler_thread_running{false};
inline std::optional<std::thread> g_handler_thread;
inline std::mutex g_handler_mutex;

/// Async-signal-safe write to stderr.
/// Uses raw write() syscall which is async-signal-safe.
inline void signal_safe_write(const char* msg, std::size_t len) {
  // Ignore return value - nothing we can do if this fails
  [[maybe_unused]] auto result = ::write(STDERR_FILENO, msg, len);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Inline implementations
// ─────────────────────────────────────────────────────────────────────────────

inline InterruptCallbackHandle::~InterruptCallbackHandle() {
  reset();
}

inline InterruptCallbackHandle::InterruptCallbackHandle(InterruptCallbackHandle&& other) noexcept
    : id_(std::exchange(other.id_, 0)) {}

inline InterruptCallbackHandle&
InterruptCallbackHandle::operator=(InterruptCallbackHandle&& other) noexcept {
  if (this != &other) {
    reset();
    id_ = std::exchange(other.id_, 0);
  }
  return *this;
}

inline void InterruptCallbackHandle::reset() {
  if (id_ != 0) {
    detail::get_callback_registry().remove(id_);
    id_ = 0;
  }
}

[[nodiscard]] inline InterruptCallbackHandle on_interrupt(std::function<void()> callback) {
  auto id = detail::get_callback_registry().add(std::move(callback));
  return InterruptCallbackHandle{id};
}

inline void invoke_interrupt_callbacks() {
  detail::get_callback_registry().invoke_all();
}

inline ReceiveInterrupts::ReceiveInterrupts()
    : target_(pthread_self()),
      callback_(on_interrupt([target = target_]() { pthread_kill(target, SIGUSR1); })) {}

inline void save_signal_mask() {
  if (!detail::g_signal_mask_saved.exchange(true, std::memory_order_acq_rel)) {
    sigset_t current;
    pthread_sigmask(SIG_BLOCK, nullptr, &current);
    detail::g_saved_signal_mask = current;
  }
}

inline void restore_signal_mask() {
  if (detail::g_signal_mask_saved.load(std::memory_order_acquire)) {
    pthread_sigmask(SIG_SETMASK, &detail::g_saved_signal_mask, nullptr);
  }
}

inline void trigger_interrupt() {
  set_interrupted(true);
  invoke_interrupt_callbacks();
}

inline bool signal_handler_running() noexcept {
  return detail::g_handler_thread_running.load(std::memory_order_acquire);
}

inline void start_signal_handler_thread(SignalConfig config) {
  std::lock_guard lock{detail::g_handler_mutex};

  if (detail::g_handler_thread_running.load(std::memory_order_acquire)) {
    return; // Already running
  }

  // Save the current signal mask before blocking
  save_signal_mask();

  // Block signals in the calling thread (inherited by child threads)
  sigset_t blocked;
  sigemptyset(&blocked);
  for (int sig : config.signals) {
    sigaddset(&blocked, sig);
  }
  pthread_sigmask(SIG_BLOCK, &blocked, nullptr);

  if (!config.start_handler_thread) {
    return;
  }

  // Start the signal handler thread
  detail::g_handler_thread_running.store(true, std::memory_order_release);
  detail::g_handler_thread.emplace([signals = std::move(config.signals)]() {
    // Block SIGUSR1 in this thread so sigtimedwait can receive it
    sigset_t block_usr1;
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block_usr1, nullptr);

    sigset_t wait_set;
    sigemptyset(&wait_set);
    for (int sig : signals) {
      sigaddset(&wait_set, sig);
    }
    // Add SIGUSR1 as a wake-up signal for clean shutdown
    sigaddset(&wait_set, SIGUSR1);

    while (detail::g_handler_thread_running.load(std::memory_order_acquire)) {
      int sig;
      timespec timeout{.tv_sec = 1, .tv_nsec = 0};
      int result = sigtimedwait(&wait_set, nullptr, &timeout);

      if (result > 0) {
        sig = result;
        // SIGUSR1 is used for wake-up during shutdown, ignore it
        if (sig == SIGUSR1) {
          continue;
        }
        // Handle the signal
        if (sig == SIGINT || sig == SIGTERM || sig == SIGHUP) {
          // Set interrupt flag (async-signal-safe)
          detail::g_interrupted.store(true, std::memory_order_release);
          // Invoke callbacks (not async-signal-safe, but we're in a thread)
          invoke_interrupt_callbacks();
        }
      }
      // On timeout or EINTR, just loop and check if we should stop
    }
  });
}

inline void stop_signal_handler_thread() {
  std::lock_guard lock{detail::g_handler_mutex};

  if (!detail::g_handler_thread_running.load(std::memory_order_acquire)) {
    return;
  }

  detail::g_handler_thread_running.store(false, std::memory_order_release);

  if (detail::g_handler_thread && detail::g_handler_thread->joinable()) {
    // Send a signal to wake up the thread from sigtimedwait
    pthread_kill(detail::g_handler_thread->native_handle(), SIGUSR1);
    detail::g_handler_thread->join();
    detail::g_handler_thread.reset();
  }
}

inline ScopedSignalBlock::ScopedSignalBlock() {
  sigfillset(&blocked_);
  pthread_sigmask(SIG_BLOCK, &blocked_, &previous_);
}

inline ScopedSignalBlock::ScopedSignalBlock(std::initializer_list<int> signals) {
  sigemptyset(&blocked_);
  for (int sig : signals) {
    sigaddset(&blocked_, sig);
  }
  pthread_sigmask(SIG_BLOCK, &blocked_, &previous_);
}

inline ScopedSignalBlock::ScopedSignalBlock(const std::vector<int>& signals) {
  sigemptyset(&blocked_);
  for (int sig : signals) {
    sigaddset(&blocked_, sig);
  }
  pthread_sigmask(SIG_BLOCK, &blocked_, &previous_);
}

inline ScopedSignalBlock::~ScopedSignalBlock() {
  pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
}

inline ScopedSignalUnblock::ScopedSignalUnblock() {
  sigset_t unblock;
  sigfillset(&unblock);
  pthread_sigmask(SIG_UNBLOCK, &unblock, &previous_);
}

inline ScopedSignalUnblock::ScopedSignalUnblock(std::initializer_list<int> signals) {
  sigset_t unblock;
  sigemptyset(&unblock);
  for (int sig : signals) {
    sigaddset(&unblock, sig);
  }
  pthread_sigmask(SIG_UNBLOCK, &unblock, &previous_);
}

inline ScopedSignalUnblock::ScopedSignalUnblock(const std::vector<int>& signals) {
  sigset_t unblock;
  sigemptyset(&unblock);
  for (int sig : signals) {
    sigaddset(&unblock, sig);
  }
  pthread_sigmask(SIG_UNBLOCK, &unblock, &previous_);
}

inline ScopedSignalUnblock::~ScopedSignalUnblock() {
  pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
}

} // namespace straylight::nix::sync
