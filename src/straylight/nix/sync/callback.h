// straylight::nix::primitives::callback
//
// A modern C++23 one-shot callback primitive that wraps std::promise/std::future.
// Replaces nix/util/callback.h with cleaner semantics and move-only design.
//
// Features:
// - One-shot semantics enforced via std::call_once
// - Thread-safe value/exception delivery
// - Move-only (non-copyable)
// - Integrates with std::future for async result retrieval

#pragma once

#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace straylight::nix::sync {

/// Exception thrown when a callback is invoked more than once.
class callback_already_invoked : public std::logic_error {
public:
  callback_already_invoked() : std::logic_error("Callback has already been invoked") {}
};

/// A one-shot callback that delivers either a value of type T or an exception.
///
/// Thread-safe: can be invoked from any thread, but only once.
/// Move-only: cannot be copied.
///
/// Usage:
///   Callback<int> cb;
///   auto future = cb.get_future();
///   cb(42);  // or cb.rethrow(std::make_exception_ptr(MyError{}));
///   int result = future.get();  // returns 42 or throws
///
template <typename T>
class Callback {
public:
  Callback() = default;

  // Move-only semantics
  Callback(const Callback&) = delete;
  Callback& operator=(const Callback&) = delete;

  Callback(Callback&& other) noexcept
      : promise_(std::move(other.promise_)), invoked_(other.invoked_.load()) {
    // Mark the source as invoked to prevent double-use after move
    other.invoked_.store(true);
  }

  Callback& operator=(Callback&& other) noexcept {
    if (this != &other) {
      promise_ = std::move(other.promise_);
      invoked_.store(other.invoked_.load());
      other.invoked_.store(true);
    }
    return *this;
  }

  /// Get the future associated with this callback.
  /// Must be called before the callback is invoked.
  [[nodiscard]] std::future<T> get_future() { return promise_.get_future(); }

  /// Fulfill the callback with a value.
  /// Throws callback_already_invoked if called more than once.
  void operator()(T value) {
    ensure_single_invocation();
    promise_.set_value(std::move(value));
  }

  /// Fulfill the callback with an exception.
  /// Throws callback_already_invoked if called more than once.
  void rethrow(std::exception_ptr exc = std::current_exception()) {
    ensure_single_invocation();
    promise_.set_exception(std::move(exc));
  }

  /// Check if the callback has been invoked.
  [[nodiscard]] bool invoked() const noexcept { return invoked_.load(std::memory_order_acquire); }

private:
  void ensure_single_invocation() {
    bool expected = false;
    if (!invoked_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      throw callback_already_invoked{};
    }
  }

  std::promise<T> promise_;
  std::atomic<bool> invoked_{false};
};

/// Specialization for void - callbacks that signal completion without a value.
template <>
class Callback<void> {
public:
  Callback() = default;

  // Move-only semantics
  Callback(const Callback&) = delete;
  Callback& operator=(const Callback&) = delete;

  Callback(Callback&& other) noexcept
      : promise_(std::move(other.promise_)), invoked_(other.invoked_.load()) {
    other.invoked_.store(true);
  }

  Callback& operator=(Callback&& other) noexcept {
    if (this != &other) {
      promise_ = std::move(other.promise_);
      invoked_.store(other.invoked_.load());
      other.invoked_.store(true);
    }
    return *this;
  }

  /// Get the future associated with this callback.
  [[nodiscard]] std::future<void> get_future() { return promise_.get_future(); }

  /// Fulfill the callback (signal completion).
  /// Throws callback_already_invoked if called more than once.
  void operator()() {
    ensure_single_invocation();
    promise_.set_value();
  }

  /// Fulfill the callback with an exception.
  /// Throws callback_already_invoked if called more than once.
  void rethrow(std::exception_ptr exc = std::current_exception()) {
    ensure_single_invocation();
    promise_.set_exception(std::move(exc));
  }

  /// Check if the callback has been invoked.
  [[nodiscard]] bool invoked() const noexcept { return invoked_.load(std::memory_order_acquire); }

private:
  void ensure_single_invocation() {
    bool expected = false;
    if (!invoked_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      throw callback_already_invoked{};
    }
  }

  std::promise<void> promise_;
  std::atomic<bool> invoked_{false};
};

} // namespace straylight::nix::sync
