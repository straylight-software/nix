// straylight::nix::primitives::finally - Scope guard primitives
//
// Modern C++23 scope guards replacing nix/util/finally.h.
// Provides three variants:
//   - Finally<F>     - always runs action on destruction
//   - ScopeSuccess<F> - runs action only on normal exit (no exceptions)
//   - ScopeFail<F>    - runs action only on exception unwind

#pragma once

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace straylight::nix::util {

// ─────────────────────────────────────────────────────────────────────────────
// Finally<F> - always runs on destruction unless dismissed
// ─────────────────────────────────────────────────────────────────────────────

/// A scope guard that runs a callable on destruction.
///
/// The callable is invoked when the Finally object goes out of scope,
/// unless dismiss() has been called or the action has been moved from.
///
/// Usage:
///   auto guard = finally([&] { cleanup(); });
///   // ... do work ...
///   guard.dismiss(); // optional: cancel cleanup
template <typename F>
class [[nodiscard("Finally values must be used - assign to a variable")]] Finally {
public:
  /// Construct from a callable.
  explicit Finally(F f) noexcept(std::is_nothrow_move_constructible_v<F>) : action_(std::move(f)) {}

  // Non-copyable
  Finally(const Finally&) = delete;
  Finally& operator=(const Finally&) = delete;

  /// Move constructor. Transfers ownership and dismisses the source.
  Finally(Finally&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
      : action_(std::move(other.action_)) {
    other.action_.reset();
  }

  /// Move assignment. Runs current action (if any), then transfers.
  Finally& operator=(Finally&& other) noexcept(std::is_nothrow_move_constructible_v<F>) {
    if (this != &other) {
      execute();
      action_ = std::move(other.action_);
      other.action_.reset();
    }
    return *this;
  }

  /// Destructor. Runs the action if not dismissed.
  ~Finally() noexcept(std::is_nothrow_invocable_v<F>) { execute(); }

  /// Cancel the action - it will not run on destruction.
  void dismiss() noexcept { action_.reset(); }

  /// Replace the action with a new one of the same type.
  /// The old action is discarded without being run.
  /// Note: For type-erased actions (different lambda types), use std::function<void()>.
  void reset(F new_action) noexcept(std::is_nothrow_move_assignable_v<F>) {
    action_ = std::move(new_action);
  }

  /// Check if the guard is still active (not dismissed).
  [[nodiscard]] bool active() const noexcept { return action_.has_value(); }

private:
  void execute() noexcept(std::is_nothrow_invocable_v<F>) {
    if (action_) {
      (*action_)();
      action_.reset();
    }
  }

  std::optional<F> action_;
};

// Deduction guide
template <typename F>
Finally(F) -> Finally<F>;

// ─────────────────────────────────────────────────────────────────────────────
// ScopeSuccess<F> - runs only on normal exit (no exception)
// ─────────────────────────────────────────────────────────────────────────────

/// A scope guard that runs a callable only on normal scope exit.
///
/// If an exception is in flight when the destructor runs, the action
/// is skipped. Uses std::uncaught_exceptions() for detection.
///
/// Usage:
///   auto guard = scope_success([&] { commit(); });
///   // ... do work that might throw ...
///   // commit() runs only if we get here without exception
template <typename F>
class [[nodiscard("ScopeSuccess values must be used - assign to a variable")]] ScopeSuccess {
public:
  /// Construct from a callable. Captures current exception count.
  explicit ScopeSuccess(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
      : action_(std::move(f)), exception_count_(std::uncaught_exceptions()) {}

  // Non-copyable
  ScopeSuccess(const ScopeSuccess&) = delete;
  ScopeSuccess& operator=(const ScopeSuccess&) = delete;

  /// Move constructor. Transfers ownership and dismisses the source.
  ScopeSuccess(ScopeSuccess&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
      : action_(std::move(other.action_)), exception_count_(other.exception_count_) {
    other.action_.reset();
  }

  /// Move assignment.
  ScopeSuccess& operator=(ScopeSuccess&& other) noexcept(std::is_nothrow_move_constructible_v<F>) {
    if (this != &other) {
      // Don't execute - we're being reassigned
      action_ = std::move(other.action_);
      exception_count_ = other.exception_count_;
      other.action_.reset();
    }
    return *this;
  }

  /// Destructor. Runs action only if no new exceptions since construction.
  ~ScopeSuccess() noexcept(std::is_nothrow_invocable_v<F>) {
    if (action_ && std::uncaught_exceptions() == exception_count_) {
      (*action_)();
    }
  }

  /// Cancel the action.
  void dismiss() noexcept { action_.reset(); }

  /// Replace the action with a new one of the same type.
  /// The old action is discarded without being run.
  void reset(F new_action) noexcept(std::is_nothrow_move_assignable_v<F>) {
    action_ = std::move(new_action);
  }

  /// Check if the guard is still active.
  [[nodiscard]] bool active() const noexcept { return action_.has_value(); }

private:
  std::optional<F> action_;
  int exception_count_;
};

// Deduction guide
template <typename F>
ScopeSuccess(F) -> ScopeSuccess<F>;

// ─────────────────────────────────────────────────────────────────────────────
// ScopeFail<F> - runs only on exception
// ─────────────────────────────────────────────────────────────────────────────

/// A scope guard that runs a callable only on exception unwind.
///
/// If an exception is in flight when the destructor runs that wasn't
/// present at construction, the action runs. Uses std::uncaught_exceptions().
///
/// Usage:
///   auto guard = scope_fail([&] { rollback(); });
///   // ... do work that might throw ...
///   // rollback() runs only if an exception is thrown
template <typename F>
class [[nodiscard("ScopeFail values must be used - assign to a variable")]] ScopeFail {
public:
  /// Construct from a callable. Captures current exception count.
  explicit ScopeFail(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
      : action_(std::move(f)), exception_count_(std::uncaught_exceptions()) {}

  // Non-copyable
  ScopeFail(const ScopeFail&) = delete;
  ScopeFail& operator=(const ScopeFail&) = delete;

  /// Move constructor. Transfers ownership and dismisses the source.
  ScopeFail(ScopeFail&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
      : action_(std::move(other.action_)), exception_count_(other.exception_count_) {
    other.action_.reset();
  }

  /// Move assignment.
  ScopeFail& operator=(ScopeFail&& other) noexcept(std::is_nothrow_move_constructible_v<F>) {
    if (this != &other) {
      // Don't execute - we're being reassigned
      action_ = std::move(other.action_);
      exception_count_ = other.exception_count_;
      other.action_.reset();
    }
    return *this;
  }

  /// Destructor. Runs action only if new exceptions since construction.
  /// Note: The action itself should not throw during exception unwinding.
  ~ScopeFail() noexcept {
    if (action_ && std::uncaught_exceptions() > exception_count_) {
      try {
        (*action_)();
      } catch (...) {
        // Swallow exceptions during unwind to avoid std::terminate
      }
    }
  }

  /// Cancel the action.
  void dismiss() noexcept { action_.reset(); }

  /// Replace the action with a new one of the same type.
  /// The old action is discarded without being run.
  void reset(F new_action) noexcept(std::is_nothrow_move_assignable_v<F>) {
    action_ = std::move(new_action);
  }

  /// Check if the guard is still active.
  [[nodiscard]] bool active() const noexcept { return action_.has_value(); }

private:
  std::optional<F> action_;
  int exception_count_;
};

// Deduction guide
template <typename F>
ScopeFail(F) -> ScopeFail<F>;

// ─────────────────────────────────────────────────────────────────────────────
// Factory functions
// ─────────────────────────────────────────────────────────────────────────────

/// Create a Finally guard that runs the given action on scope exit.
///
/// Example:
///   auto guard = finally([&] { cleanup(); });
template <typename F>
  requires std::is_invocable_v<F>
[[nodiscard]] auto finally(F&& f) noexcept(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>) {
  return Finally<std::decay_t<F>>(std::forward<F>(f));
}

/// Create a ScopeSuccess guard that runs only on normal exit.
///
/// Example:
///   auto guard = scope_success([&] { commit(); });
template <typename F>
  requires std::is_invocable_v<F>
[[nodiscard]] auto
scope_success(F&& f) noexcept(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>) {
  return ScopeSuccess<std::decay_t<F>>(std::forward<F>(f));
}

/// Create a ScopeFail guard that runs only on exception.
///
/// Example:
///   auto guard = scope_fail([&] { rollback(); });
template <typename F>
  requires std::is_invocable_v<F>
[[nodiscard]] auto
scope_fail(F&& f) noexcept(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>) {
  return ScopeFail<std::decay_t<F>>(std::forward<F>(f));
}

} // namespace straylight::nix::util
