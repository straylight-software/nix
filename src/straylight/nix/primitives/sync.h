// straylight::nix::primitives::sync
//
// Synchronized value wrapper primitive for thread-safe access to shared data.
// Similar to folly::Synchronized, provides RAII-based locking with proxy objects.
//
// Features:
// - Sync<T, Mutex> for exclusive access (std::mutex by default)
// - SharedSync<T> for reader-writer semantics (std::shared_mutex)
// - Lock proxy with operator-> and operator* for safe access
// - try_lock() for non-blocking acquisition
// - with_lock(callable) for scoped operations
// - Condition variable support (wait/notify)
//
// Usage:
//   Sync<Data> data;
//   {
//     auto lock = data.lock();
//     lock->x = 42;
//   }
//
//   data.with_lock([](Data& d) { d.x = 42; });
//
//   SharedSync<Data> shared_data;
//   {
//     auto read = shared_data.read_lock();
//     std::cout << read->x;
//   }

#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <utility>

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

template <typename T, typename Mutex>
class SyncBase;

template <typename T>
using Sync = SyncBase<T, std::mutex>;

template <typename T>
using SharedSync = SyncBase<T, std::shared_mutex>;

// ─────────────────────────────────────────────────────────────────────────────
// Lock type traits
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// Detect if mutex supports shared locking
template <typename M>
concept SharedLockable = requires(M& m) {
  m.lock_shared();
  m.unlock_shared();
  m.try_lock_shared();
};

// Lock type selection based on mutex type
template <typename Mutex>
using WriteLockType = std::unique_lock<Mutex>;

template <typename Mutex>
using ReadLockType =
    std::conditional_t<SharedLockable<Mutex>, std::shared_lock<Mutex>, std::unique_lock<Mutex>>;

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Lock proxy classes
// ─────────────────────────────────────────────────────────────────────────────

/// Write lock proxy providing exclusive mutable access to the wrapped value.
/// Holds a unique_lock and provides operator-> and operator* for access.
template <typename T, typename Mutex>
class WriteLock {
public:
  using value_type = T;
  using mutex_type = Mutex;
  using lock_type = detail::WriteLockType<Mutex>;

  WriteLock(WriteLock&&) noexcept = default;
  WriteLock& operator=(WriteLock&&) noexcept = default;
  WriteLock(const WriteLock&) = delete;
  WriteLock& operator=(const WriteLock&) = delete;

  ~WriteLock() = default;

  /// Access the wrapped value via pointer
  [[nodiscard]] T* operator->() noexcept { return ptr_; }
  [[nodiscard]] const T* operator->() const noexcept { return ptr_; }

  /// Access the wrapped value via reference
  [[nodiscard]] T& operator*() noexcept { return *ptr_; }
  [[nodiscard]] const T& operator*() const noexcept { return *ptr_; }

  /// Get raw pointer to the wrapped value
  [[nodiscard]] T* get() noexcept { return ptr_; }
  [[nodiscard]] const T* get() const noexcept { return ptr_; }

  /// Check if lock is held
  [[nodiscard]] bool owns_lock() const noexcept { return lock_.owns_lock(); }

  /// Release the lock early (value becomes inaccessible)
  void unlock() {
    lock_.unlock();
    ptr_ = nullptr;
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Condition variable support
  // ───────────────────────────────────────────────────────────────────────────

  /// Wait on a condition variable
  void wait(std::condition_variable& cv) {
    assert(owns_lock());
    cv.wait(lock_);
  }

  /// Wait on a condition variable with predicate
  template <typename Predicate>
  void wait(std::condition_variable& cv, Predicate pred) {
    assert(owns_lock());
    cv.wait(lock_, std::move(pred));
  }

  /// Wait on a condition variable with timeout
  template <typename Rep, typename Period>
  std::cv_status wait_for(std::condition_variable& cv,
                          const std::chrono::duration<Rep, Period>& timeout) {
    assert(owns_lock());
    return cv.wait_for(lock_, timeout);
  }

  /// Wait on a condition variable with timeout and predicate
  template <typename Rep, typename Period, typename Predicate>
  bool wait_for(std::condition_variable& cv, const std::chrono::duration<Rep, Period>& timeout,
                Predicate pred) {
    assert(owns_lock());
    return cv.wait_for(lock_, timeout, std::move(pred));
  }

  /// Wait on a condition variable until a time point
  template <typename Clock, typename Duration>
  std::cv_status wait_until(std::condition_variable& cv,
                            const std::chrono::time_point<Clock, Duration>& deadline) {
    assert(owns_lock());
    return cv.wait_until(lock_, deadline);
  }

  /// Wait on a condition variable until a time point with predicate
  template <typename Clock, typename Duration, typename Predicate>
  bool wait_until(std::condition_variable& cv,
                  const std::chrono::time_point<Clock, Duration>& deadline, Predicate pred) {
    assert(owns_lock());
    return cv.wait_until(lock_, deadline, std::move(pred));
  }

private:
  template <typename, typename>
  friend class SyncBase;

  WriteLock(T* ptr, Mutex& mutex) : ptr_(ptr), lock_(mutex) {}

  WriteLock(T* ptr, Mutex& mutex, std::try_to_lock_t) : ptr_(ptr), lock_(mutex, std::try_to_lock) {
    if (!lock_.owns_lock()) {
      ptr_ = nullptr;
    }
  }

  T* ptr_;
  lock_type lock_;
};

/// Read lock proxy providing shared const access to the wrapped value.
/// Uses shared_lock for shared_mutex, unique_lock otherwise.
template <typename T, typename Mutex>
class ReadLock {
public:
  using value_type = T;
  using mutex_type = Mutex;
  using lock_type = detail::ReadLockType<Mutex>;

  ReadLock(ReadLock&&) noexcept = default;
  ReadLock& operator=(ReadLock&&) noexcept = default;
  ReadLock(const ReadLock&) = delete;
  ReadLock& operator=(const ReadLock&) = delete;

  ~ReadLock() = default;

  /// Access the wrapped value via pointer (const only)
  [[nodiscard]] const T* operator->() const noexcept { return ptr_; }

  /// Access the wrapped value via reference (const only)
  [[nodiscard]] const T& operator*() const noexcept { return *ptr_; }

  /// Get raw pointer to the wrapped value
  [[nodiscard]] const T* get() const noexcept { return ptr_; }

  /// Check if lock is held
  [[nodiscard]] bool owns_lock() const noexcept { return lock_.owns_lock(); }

  /// Release the lock early (value becomes inaccessible)
  void unlock() {
    lock_.unlock();
    ptr_ = nullptr;
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Condition variable support (for shared_mutex with condition_variable_any)
  // ───────────────────────────────────────────────────────────────────────────

  /// Wait on a condition_variable_any
  void wait(std::condition_variable_any& cv) {
    assert(owns_lock());
    cv.wait(lock_);
  }

  /// Wait on a condition_variable_any with predicate
  template <typename Predicate>
  void wait(std::condition_variable_any& cv, Predicate pred) {
    assert(owns_lock());
    cv.wait(lock_, std::move(pred));
  }

  /// Wait on a condition_variable_any with timeout
  template <typename Rep, typename Period>
  std::cv_status wait_for(std::condition_variable_any& cv,
                          const std::chrono::duration<Rep, Period>& timeout) {
    assert(owns_lock());
    return cv.wait_for(lock_, timeout);
  }

private:
  template <typename, typename>
  friend class SyncBase;

  ReadLock(const T* ptr, Mutex& mutex) : ptr_(ptr), lock_(mutex) {}

  ReadLock(const T* ptr, Mutex& mutex, std::try_to_lock_t)
      : ptr_(ptr), lock_(mutex, std::try_to_lock) {
    if (!lock_.owns_lock()) {
      ptr_ = nullptr;
    }
  }

  const T* ptr_;
  lock_type lock_;
};

// ─────────────────────────────────────────────────────────────────────────────
// SyncBase - main synchronized value wrapper
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe wrapper for a value of type T using mutex type Mutex.
///
/// Provides RAII-based locking through lock proxy objects that automatically
/// release the mutex when destroyed. The proxy objects provide operator-> and
/// operator* for convenient access to the wrapped value.
///
/// For read-only access with std::shared_mutex, use read_lock() which returns
/// a ReadLock holding a shared_lock.
template <typename T, typename Mutex = std::mutex>
class SyncBase {
public:
  using value_type = T;
  using mutex_type = Mutex;
  using write_lock_type = WriteLock<T, Mutex>;
  using read_lock_type = ReadLock<T, Mutex>;

  // ───────────────────────────────────────────────────────────────────────────
  // Construction
  // ───────────────────────────────────────────────────────────────────────────

  /// Default construct the wrapped value (value-initialized)
  SyncBase()
    requires std::is_default_constructible_v<T>
      : data_{} {}

  /// Copy construct the wrapped value
  explicit SyncBase(const T& value)
    requires std::is_copy_constructible_v<T>
      : data_(value) {}

  /// Move construct the wrapped value
  explicit SyncBase(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)
    requires std::is_move_constructible_v<T>
      : data_(std::move(value)) {}

  /// In-place construct the wrapped value
  template <typename... Args>
    requires std::is_constructible_v<T, Args...>
  explicit SyncBase(std::in_place_t, Args&&... args) : data_(std::forward<Args>(args)...) {}

  // ───────────────────────────────────────────────────────────────────────────
  // Copy/Move (locks both sides for safety)
  // ───────────────────────────────────────────────────────────────────────────

  /// Copy constructor - locks the source
  SyncBase(const SyncBase& other)
    requires std::is_copy_constructible_v<T>
  {
    auto lock = other.lock_impl();
    data_ = other.data_;
  }

  /// Move constructor - locks the source
  SyncBase(SyncBase&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
    requires std::is_move_constructible_v<T>
  {
    auto lock = other.lock_impl();
    data_ = std::move(other.data_);
  }

  /// Copy assignment - locks both sides
  SyncBase& operator=(const SyncBase& other)
    requires std::is_copy_assignable_v<T>
  {
    if (this != &other) {
      // Lock in consistent order to avoid deadlock
      std::scoped_lock locks(mutex_, other.mutex_);
      data_ = other.data_;
    }
    return *this;
  }

  /// Move assignment - locks both sides
  SyncBase& operator=(SyncBase&& other) noexcept(std::is_nothrow_move_assignable_v<T>)
    requires std::is_move_assignable_v<T>
  {
    if (this != &other) {
      std::scoped_lock locks(mutex_, other.mutex_);
      data_ = std::move(other.data_);
    }
    return *this;
  }

  ~SyncBase() = default;

  // ───────────────────────────────────────────────────────────────────────────
  // Locking interface
  // ───────────────────────────────────────────────────────────────────────────

  /// Acquire exclusive (write) access to the wrapped value.
  /// Returns a WriteLock proxy that releases the mutex on destruction.
  [[nodiscard]] write_lock_type lock() { return write_lock_type(&data_, mutex_); }

  /// Acquire exclusive (write) access to the wrapped value (const version for mutable mutex).
  [[nodiscard]] write_lock_type lock() const {
    return write_lock_type(const_cast<T*>(&data_), const_cast<Mutex&>(mutex_));
  }

  /// Try to acquire exclusive (write) access without blocking.
  /// Returns std::nullopt if the lock cannot be acquired immediately.
  [[nodiscard]] std::optional<write_lock_type> try_lock() {
    write_lock_type proxy(&data_, mutex_, std::try_to_lock);
    if (proxy.owns_lock()) {
      return std::move(proxy);
    }
    return std::nullopt;
  }

  /// Acquire shared (read) access to the wrapped value.
  /// For std::shared_mutex, multiple readers can hold the lock simultaneously.
  /// For std::mutex, this is equivalent to lock().
  [[nodiscard]] read_lock_type read_lock() const {
    return read_lock_type(&data_, const_cast<Mutex&>(mutex_));
  }

  /// Try to acquire shared (read) access without blocking.
  [[nodiscard]] std::optional<read_lock_type> try_read_lock() const {
    read_lock_type proxy(&data_, const_cast<Mutex&>(mutex_), std::try_to_lock);
    if (proxy.owns_lock()) {
      return std::move(proxy);
    }
    return std::nullopt;
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Scoped operation helpers
  // ───────────────────────────────────────────────────────────────────────────

  /// Execute a callable with exclusive access to the wrapped value.
  /// Returns the result of the callable.
  template <typename F>
    requires std::invocable<F, T&>
  auto with_lock(F&& func) -> std::invoke_result_t<F, T&> {
    auto proxy = lock();
    return std::invoke(std::forward<F>(func), *proxy);
  }

  /// Execute a callable with exclusive access to the wrapped value (const version).
  template <typename F>
    requires std::invocable<F, T&>
  auto with_lock(F&& func) const -> std::invoke_result_t<F, T&> {
    auto proxy = lock();
    return std::invoke(std::forward<F>(func), *proxy);
  }

  /// Execute a callable with shared (read) access to the wrapped value.
  template <typename F>
    requires std::invocable<F, const T&>
  auto with_read_lock(F&& func) const -> std::invoke_result_t<F, const T&> {
    auto proxy = read_lock();
    return std::invoke(std::forward<F>(func), *proxy);
  }

  /// Try to execute a callable with exclusive access.
  /// Returns std::nullopt if the lock cannot be acquired.
  template <typename F>
    requires std::invocable<F, T&>
  auto try_with_lock(F&& func) -> std::optional<std::invoke_result_t<F, T&>> {
    if (auto proxy = try_lock()) {
      return std::invoke(std::forward<F>(func), **proxy);
    }
    return std::nullopt;
  }

  /// Try to execute a callable with shared (read) access.
  /// Returns std::nullopt if the lock cannot be acquired.
  template <typename F>
    requires std::invocable<F, const T&>
  auto try_with_read_lock(F&& func) const -> std::optional<std::invoke_result_t<F, const T&>> {
    if (auto proxy = try_read_lock()) {
      return std::invoke(std::forward<F>(func), **proxy);
    }
    return std::nullopt;
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Unsafe access (use with caution)
  // ───────────────────────────────────────────────────────────────────────────

  /// Get a reference to the underlying value without locking.
  /// WARNING: This is unsafe and should only be used when external
  /// synchronization is guaranteed (e.g., during single-threaded setup).
  [[nodiscard]] T& unsafe_get() noexcept { return data_; }
  [[nodiscard]] const T& unsafe_get() const noexcept { return data_; }

  /// Get a reference to the underlying mutex.
  /// Useful for coordinating with external locking mechanisms.
  [[nodiscard]] Mutex& mutex() const noexcept { return const_cast<Mutex&>(mutex_); }

  // ───────────────────────────────────────────────────────────────────────────
  // Copy/swap helpers
  // ───────────────────────────────────────────────────────────────────────────

  /// Get a copy of the wrapped value
  [[nodiscard]] T copy() const
    requires std::is_copy_constructible_v<T>
  {
    auto proxy = read_lock();
    return *proxy;
  }

  /// Swap the wrapped value
  void swap(T& other) {
    auto proxy = lock();
    using std::swap;
    swap(*proxy, other);
  }

  /// Swap with another SyncBase
  void swap(SyncBase& other) {
    if (this != &other) {
      std::scoped_lock locks(mutex_, other.mutex_);
      using std::swap;
      swap(data_, other.data_);
    }
  }

private:
  /// Internal lock helper for copy/move constructors
  [[nodiscard]] detail::WriteLockType<Mutex> lock_impl() const {
    return detail::WriteLockType<Mutex>(const_cast<Mutex&>(mutex_));
  }

  mutable Mutex mutex_;
  T data_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Free functions
// ─────────────────────────────────────────────────────────────────────────────

/// Swap two SyncBase objects
template <typename T, typename Mutex>
void swap(SyncBase<T, Mutex>& lhs, SyncBase<T, Mutex>& rhs) {
  lhs.swap(rhs);
}

// ─────────────────────────────────────────────────────────────────────────────
// Synchronized pair for condition variable patterns
// ─────────────────────────────────────────────────────────────────────────────

/// A Sync<T> bundled with a condition variable for wait/notify patterns.
template <typename T, typename Mutex = std::mutex>
class SyncWithCV {
public:
  using sync_type = SyncBase<T, Mutex>;
  using value_type = T;
  using mutex_type = Mutex;
  using write_lock_type = typename sync_type::write_lock_type;
  using read_lock_type = typename sync_type::read_lock_type;

  SyncWithCV()
    requires std::is_default_constructible_v<T>
  = default;

  explicit SyncWithCV(const T& value)
    requires std::is_copy_constructible_v<T>
      : sync_(value) {}

  explicit SyncWithCV(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)
    requires std::is_move_constructible_v<T>
      : sync_(std::move(value)) {}

  template <typename... Args>
    requires std::is_constructible_v<T, Args...>
  explicit SyncWithCV(std::in_place_t, Args&&... args)
      : sync_(std::in_place, std::forward<Args>(args)...) {}

  /// Acquire exclusive lock
  [[nodiscard]] write_lock_type lock() { return sync_.lock(); }

  /// Acquire shared lock
  [[nodiscard]] read_lock_type read_lock() const { return sync_.read_lock(); }

  /// Try to acquire exclusive lock
  [[nodiscard]] std::optional<write_lock_type> try_lock() { return sync_.try_lock(); }

  /// Get the condition variable for wait/notify operations
  [[nodiscard]] std::condition_variable& cv() noexcept { return cv_; }
  [[nodiscard]] const std::condition_variable& cv() const noexcept { return cv_; }

  /// Notify one waiting thread
  void notify_one() noexcept { cv_.notify_one(); }

  /// Notify all waiting threads
  void notify_all() noexcept { cv_.notify_all(); }

  /// Wait for a condition with exclusive lock
  template <typename Predicate>
  void wait(write_lock_type& proxy, Predicate pred) {
    proxy.wait(cv_, std::move(pred));
  }

  /// Wait for a condition with timeout
  template <typename Rep, typename Period, typename Predicate>
  bool wait_for(write_lock_type& proxy, const std::chrono::duration<Rep, Period>& timeout,
                Predicate pred) {
    return proxy.wait_for(cv_, timeout, std::move(pred));
  }

  /// Execute callable with exclusive lock, then notify_one
  template <typename F>
    requires std::invocable<F, T&>
  auto with_lock_notify_one(F&& func) -> std::invoke_result_t<F, T&> {
    if constexpr (std::is_void_v<std::invoke_result_t<F, T&>>) {
      sync_.with_lock(std::forward<F>(func));
      cv_.notify_one();
    } else {
      auto result = sync_.with_lock(std::forward<F>(func));
      cv_.notify_one();
      return result;
    }
  }

  /// Execute callable with exclusive lock, then notify_all
  template <typename F>
    requires std::invocable<F, T&>
  auto with_lock_notify_all(F&& func) -> std::invoke_result_t<F, T&> {
    if constexpr (std::is_void_v<std::invoke_result_t<F, T&>>) {
      sync_.with_lock(std::forward<F>(func));
      cv_.notify_all();
    } else {
      auto result = sync_.with_lock(std::forward<F>(func));
      cv_.notify_all();
      return result;
    }
  }

  /// Access the underlying Sync object
  [[nodiscard]] sync_type& sync() noexcept { return sync_; }
  [[nodiscard]] const sync_type& sync() const noexcept { return sync_; }

private:
  sync_type sync_;
  std::condition_variable cv_;
};

} // namespace straylight::nix::primitives
