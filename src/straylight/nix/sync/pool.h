// straylight::nix::primitives::pool
//
// Thread-safe resource pool with RAII handles, blocking/non-blocking acquisition,
// health checks, and configurable initialization strategies.
//
// Modern C++23 replacement for nix/util/pool.h using std::counting_semaphore
// for efficient blocking and std::mutex + std::vector for the free list.

#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <utility>
#include <vector>

namespace straylight::nix::sync {

// ─────────────────────────────────────────────────────────────────────────────
// Pool initialization mode
// ─────────────────────────────────────────────────────────────────────────────

/// Controls when resources are created in the pool.
enum class PoolInitMode {
  /// Resources are created on-demand when first requested (default).
  Lazy,
  /// All resources are created upfront during pool construction.
  Eager,
};

// ─────────────────────────────────────────────────────────────────────────────
// Pool configuration
// ─────────────────────────────────────────────────────────────────────────────

/// Configuration for Pool construction.
template <typename T>
struct PoolConfig {
  /// Maximum number of resources in the pool.
  std::size_t max_size = 8;

  /// Factory function to create new resources.
  /// Default constructs resources using std::make_unique.
  std::function<std::unique_ptr<T>()> factory = []() { return std::make_unique<T>(); };

  /// Health check function to validate resources before reuse.
  /// Return true if the resource is healthy and can be reused.
  /// Return false to discard the resource and create a new one.
  std::function<bool(const T&)> health_check = [](const T&) { return true; };

  /// Initialization mode (lazy or eager).
  PoolInitMode init_mode = PoolInitMode::Lazy;
};

// ─────────────────────────────────────────────────────────────────────────────
// Pool
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe resource pool with RAII handles.
///
/// Example usage:
///
///   PoolConfig<Connection> config{
///     .max_size = 10,
///     .factory = []() { return std::make_unique<Connection>("localhost"); },
///     .health_check = [](const Connection& c) { return c.is_connected(); },
///   };
///   Pool<Connection> pool(config);
///
///   {
///     auto handle = pool.get();  // Blocks until resource available
///     handle->execute("SELECT ...");
///   }  // Resource returned to pool on destruction
///
template <typename T>
class Pool {
public:
  class Handle;

private:
  // Configuration
  std::function<std::unique_ptr<T>()> factory_;
  std::function<bool(const T&)> health_check_;
  std::size_t max_size_;

  // Synchronization
  // Semaphore tracks available slots (max_size - in_use)
  // Initial count = max_size (all slots available for claiming)
  std::counting_semaphore<> semaphore_;
  mutable std::mutex mutex_;

  // Free list of available resources
  std::vector<std::unique_ptr<T>> free_list_;

  // Track resources currently in use (for diagnostics)
  std::size_t in_use_ = 0;

  // Total resources created (for diagnostics)
  std::size_t created_ = 0;

public:
  /// Construct a pool with the given configuration.
  explicit Pool(PoolConfig<T> config)
      : factory_(std::move(config.factory)),
        health_check_(std::move(config.health_check)),
        max_size_(config.max_size),
        semaphore_(static_cast<std::ptrdiff_t>(config.max_size)) {
    assert(max_size_ > 0 && "Pool max_size must be positive");

    if (config.init_mode == PoolInitMode::Eager) {
      // Pre-create all resources
      free_list_.reserve(max_size_);
      for (std::size_t i = 0; i < max_size_; ++i) {
        free_list_.push_back(factory_());
        ++created_;
      }
    }
  }

  /// Construct a pool with default configuration and specified max size.
  explicit Pool(std::size_t max_size = 8) : Pool(PoolConfig<T>{.max_size = max_size}) {}

  /// Destructor. Asserts that no resources are currently in use.
  ~Pool() {
    std::lock_guard lock(mutex_);
    assert(in_use_ == 0 && "Pool destroyed with resources still in use");
  }

  // Non-copyable, non-movable (references held by handles)
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  Pool(Pool&&) = delete;
  Pool& operator=(Pool&&) = delete;

  // ───────────────────────────────────────────────────────────────────────────
  // Resource acquisition
  // ───────────────────────────────────────────────────────────────────────────

  /// Acquire a resource, blocking indefinitely until one is available.
  /// Returns an RAII Handle that returns the resource to the pool on destruction.
  [[nodiscard]] Handle get() {
    // Block until a slot is available
    semaphore_.acquire();

    return acquire_resource();
  }

  /// Acquire a resource with a timeout.
  /// Returns std::nullopt if timeout expires before a resource becomes available.
  template <typename Rep, typename Period>
  [[nodiscard]] std::optional<Handle> get(std::chrono::duration<Rep, Period> timeout) {
    // Try to acquire a slot within the timeout
    if (!semaphore_.try_acquire_for(timeout)) {
      return std::nullopt;
    }

    return acquire_resource();
  }

  /// Try to acquire a resource without blocking.
  /// Returns std::nullopt if no resource is immediately available.
  [[nodiscard]] std::optional<Handle> try_get() {
    // Try to acquire a slot immediately
    if (!semaphore_.try_acquire()) {
      return std::nullopt;
    }

    return acquire_resource();
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Pool management
  // ───────────────────────────────────────────────────────────────────────────

  /// Flush resources that fail the health check from the free list.
  /// Does not affect resources currently in use.
  void flush_bad() {
    std::lock_guard lock(mutex_);
    std::erase_if(free_list_,
                  [this](const std::unique_ptr<T>& resource) { return !health_check_(*resource); });
  }

  /// Clear all resources from the free list.
  /// Does not affect resources currently in use.
  /// Returns the cleared resources for potential cleanup.
  [[nodiscard]] std::vector<std::unique_ptr<T>> clear() {
    std::lock_guard lock(mutex_);
    return std::exchange(free_list_, {});
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Diagnostics
  // ───────────────────────────────────────────────────────────────────────────

  /// Maximum number of resources this pool can hold.
  [[nodiscard]] std::size_t max_size() const noexcept { return max_size_; }

  /// Number of resources currently in use.
  [[nodiscard]] std::size_t in_use() const {
    std::lock_guard lock(mutex_);
    return in_use_;
  }

  /// Number of resources currently idle in the free list.
  [[nodiscard]] std::size_t idle() const {
    std::lock_guard lock(mutex_);
    return free_list_.size();
  }

  /// Total number of resources created (includes discarded ones).
  [[nodiscard]] std::size_t created() const {
    std::lock_guard lock(mutex_);
    return created_;
  }

  /// Total number of resources currently managed (in_use + idle).
  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(mutex_);
    return in_use_ + free_list_.size();
  }

private:
  /// Acquire a resource from the pool. Called after semaphore is acquired.
  [[nodiscard]] Handle acquire_resource() {
    std::unique_ptr<T> resource;

    {
      std::lock_guard lock(mutex_);

      // Try to get a healthy resource from the free list
      while (!free_list_.empty()) {
        resource = std::move(free_list_.back());
        free_list_.pop_back();

        if (health_check_(*resource)) {
          // Found a healthy resource
          ++in_use_;
          return Handle(*this, std::move(resource));
        }
        // Resource failed health check, discard it and try another
        resource.reset();
      }

      // No resources in free list, create a new one
      ++in_use_;
      ++created_;
    }

    // Create resource outside the lock (may be expensive)
    resource = factory_();
    return Handle(*this, std::move(resource));
  }

  /// Return a resource to the pool. Called by Handle destructor.
  void release_resource(std::unique_ptr<T> resource, bool discard) {
    {
      std::lock_guard lock(mutex_);
      assert(in_use_ > 0 && "Release without corresponding acquire");
      --in_use_;

      if (!discard && resource) {
        free_list_.push_back(std::move(resource));
      }
    }

    // Signal that a slot is now available
    semaphore_.release();
  }

public:
  // ───────────────────────────────────────────────────────────────────────────
  // Handle
  // ───────────────────────────────────────────────────────────────────────────

  /// RAII handle to a pooled resource.
  /// Automatically returns the resource to the pool on destruction.
  class Handle {
  private:
    Pool* pool_;
    std::unique_ptr<T> resource_;
    bool discard_ = false;

    friend class Pool;

    Handle(Pool& pool, std::unique_ptr<T> resource)
        : pool_(&pool), resource_(std::move(resource)) {}

  public:
    /// Move constructor. Transfers ownership from another handle.
    Handle(Handle&& other) noexcept
        : pool_(other.pool_), resource_(std::move(other.resource_)), discard_(other.discard_) {
      other.pool_ = nullptr;
    }

    /// Move assignment. Transfers ownership from another handle.
    Handle& operator=(Handle&& other) noexcept {
      if (this != &other) {
        // Release current resource if any
        if (pool_ && resource_) {
          pool_->release_resource(std::move(resource_), discard_);
        }

        pool_ = other.pool_;
        resource_ = std::move(other.resource_);
        discard_ = other.discard_;
        other.pool_ = nullptr;
      }
      return *this;
    }

    // Non-copyable
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    /// Destructor. Returns the resource to the pool unless marked for discard.
    ~Handle() {
      if (pool_ && resource_) {
        pool_->release_resource(std::move(resource_), discard_);
      }
    }

    /// Access the underlying resource.
    [[nodiscard]] T* operator->() noexcept { return resource_.get(); }
    [[nodiscard]] const T* operator->() const noexcept { return resource_.get(); }

    /// Dereference the handle to get the underlying resource.
    [[nodiscard]] T& operator*() noexcept { return *resource_; }
    [[nodiscard]] const T& operator*() const noexcept { return *resource_; }

    /// Get a raw pointer to the underlying resource.
    [[nodiscard]] T* get() noexcept { return resource_.get(); }
    [[nodiscard]] const T* get() const noexcept { return resource_.get(); }

    /// Check if the handle holds a valid resource.
    [[nodiscard]] explicit operator bool() const noexcept { return resource_ != nullptr; }

    /// Mark the resource as bad. It will be discarded instead of returned to the pool.
    /// Use this when the resource has entered an unrecoverable error state.
    void mark_bad() noexcept { discard_ = true; }

    /// Check if the resource is marked as bad.
    [[nodiscard]] bool is_bad() const noexcept { return discard_; }
  };
};

} // namespace straylight::nix::sync
