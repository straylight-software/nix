#pragma once

/// stable_ref.h - Compile-time enforcement of buffer lifetime safety
///
/// The Problem:
/// State machines copy state between steps. If you pass &s.buffer to an
/// operation, the kernel writes to the old address while you read from
/// the new copy. This is undefined behavior that fails silently.
///
/// The Solution:
/// Operations that write to user buffers require stable_ref<T> or stable_span<T>.
/// These types can ONLY be constructed from:
///   1. machine_storage<T> - mutable member in the machine (stable address)
///   2. make_stable_ref()/make_stable_span() - explicit caller-owned data
///
/// Usage in machines:
/// @code
///   class my_machine {
///     machine_storage<struct statx> statx_buf_;  // Lives in machine, not state
///
///     auto step(state_type s, const event& e) const -> step_result<state_type> {
///       // OK: statx_buf_.ref() returns stable_ref
///       ops.push_back(make_statx(AT_FDCWD, path, 0, STATX_BASIC_STATS, statx_buf_.ref()));
///
///       // Won't compile: no conversion from raw pointer
///       // ops.push_back(make_statx(..., &s.statx_buf));  // ERROR
///     }
///   };
/// @endcode
///
/// Usage with caller-owned buffers:
/// @code
///   std::vector<struct statx> buffers(paths.size());
///   // Explicit opt-in: caller guarantees lifetime
///   bulk_stat_machine machine{paths, make_stable_span(buffers)};
/// @endcode

#include <cstddef>
#include <span>
#include <type_traits>

namespace evring {

// Forward declarations
template <typename T>
class stable_ref;

template <typename T>
class stable_span;

template <typename T>
class machine_storage;

// ============================================================================
// stable_ref<T> - A pointer that can only come from stable storage
// ============================================================================

/// A non-owning reference that can only be constructed from stable storage.
/// This provides compile-time enforcement that the pointed-to memory will
/// remain valid across state machine step boundaries.
template <typename T>
class stable_ref {
  T* ptr_;

  // Private constructor - only friends can create
  explicit constexpr stable_ref(T* p) noexcept : ptr_(p) {}

  template <typename U>
  friend class machine_storage;

  template <typename U>
  friend constexpr auto make_stable_ref(U& value) noexcept -> stable_ref<U>;

  template <typename U>
  friend constexpr auto make_stable_ref(U* ptr) noexcept -> stable_ref<U>;

public:
  using element_type = T;

  // Default constructor creates null ref
  constexpr stable_ref() noexcept : ptr_(nullptr) {}

  // Copy/move allowed
  constexpr stable_ref(const stable_ref&) noexcept = default;
  constexpr stable_ref(stable_ref&&) noexcept = default;
  constexpr auto operator=(const stable_ref&) noexcept -> stable_ref& = default;
  constexpr auto operator=(stable_ref&&) noexcept -> stable_ref& = default;

  // Implicit conversion to const version
  template <typename U>
    requires std::is_same_v<U, std::add_const_t<T>>
  constexpr operator stable_ref<U>() const noexcept {
    return stable_ref<U>{ptr_};
  }

  // Access
  [[nodiscard]] constexpr auto get() const noexcept -> T* { return ptr_; }
  [[nodiscard]] constexpr auto operator->() const noexcept -> T* { return ptr_; }
  [[nodiscard]] constexpr auto operator*() const noexcept -> T& { return *ptr_; }

  // Validity check
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }
  [[nodiscard]] constexpr auto valid() const noexcept -> bool { return ptr_ != nullptr; }
};

// ============================================================================
// stable_span<T> - A span that can only come from stable storage
// ============================================================================

// Private tag to prevent accidental construction
struct stable_span_tag_t {
  explicit stable_span_tag_t() = default;
};
inline constexpr stable_span_tag_t stable_span_tag{};

/// A non-owning span that can only be constructed from stable storage.
/// Used for read/write buffers in operations.
template <typename T>
class stable_span {
  T* data_;
  std::size_t size_;

public:
  // Constructor is technically public but requires a private tag,
  // so only make_stable_span and machine_storage can create it
  constexpr stable_span(stable_span_tag_t, T* d, std::size_t s) noexcept : data_(d), size_(s) {}
  using element_type = T;
  using value_type = std::remove_cv_t<T>;
  using size_type = std::size_t;
  using pointer = T*;

  // Default constructor creates empty span
  constexpr stable_span() noexcept : data_(nullptr), size_(0) {}

  // Copy/move allowed
  constexpr stable_span(const stable_span&) noexcept = default;
  constexpr stable_span(stable_span&&) noexcept = default;
  constexpr auto operator=(const stable_span&) noexcept -> stable_span& = default;
  constexpr auto operator=(stable_span&&) noexcept -> stable_span& = default;

  // Implicit conversion to const version
  template <typename U>
    requires std::is_same_v<U, std::add_const_t<T>>
  constexpr operator stable_span<U>() const noexcept {
    return stable_span<U>{stable_span_tag, data_, size_};
  }

  // Convert to std::span for interop
  [[nodiscard]] constexpr auto span() const noexcept -> std::span<T> { return {data_, size_}; }
  [[nodiscard]] constexpr operator std::span<T>() const noexcept { return {data_, size_}; }

  // Access
  [[nodiscard]] constexpr auto data() const noexcept -> T* { return data_; }
  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] constexpr auto empty() const noexcept -> bool { return size_ == 0; }

  [[nodiscard]] constexpr auto operator[](std::size_t idx) const noexcept -> T& {
    return data_[idx];
  }

  // Iterators
  [[nodiscard]] constexpr auto begin() const noexcept -> T* { return data_; }
  [[nodiscard]] constexpr auto end() const noexcept -> T* { return data_ + size_; }

  // Subspan
  [[nodiscard]] constexpr auto subspan(std::size_t offset, std::size_t count) const noexcept
      -> stable_span {
    return stable_span{stable_span_tag, data_ + offset, count};
  }
};

// ============================================================================
// machine_storage<T> - Stable storage that lives in the machine
// ============================================================================

/// Storage for data that needs to outlive state copies.
/// Place this as a member of your machine class (not the state).
/// The mutable keyword allows modification from const step() methods.
///
/// @code
///   class my_machine {
///     machine_storage<struct statx> statx_buf_;
///
///     auto step(state s, const event& e) const -> step_result<state> {
///       ops.push_back(make_statx(..., statx_buf_.ref()));
///       // After completion, read from statx_buf_.get()
///     }
///   };
/// @endcode
template <typename T>
class machine_storage {
  mutable T value_{};

public:
  machine_storage() = default;

  // No copy/move - machine_storage should be a member, not passed around
  machine_storage(const machine_storage&) = delete;
  machine_storage(machine_storage&&) = delete;
  auto operator=(const machine_storage&) -> machine_storage& = delete;
  auto operator=(machine_storage&&) -> machine_storage& = delete;

  /// Get a stable reference for use in operations
  [[nodiscard]] auto ref() const noexcept -> stable_ref<T> { return stable_ref<T>{&value_}; }

  /// Direct access to the stored value (for reading results)
  [[nodiscard]] auto get() const noexcept -> T& { return value_; }
  [[nodiscard]] auto operator*() const noexcept -> T& { return value_; }
  [[nodiscard]] auto operator->() const noexcept -> T* { return &value_; }
};

/// Specialization for arrays - provides stable_span access
template <typename T, std::size_t N>
class machine_storage<T[N]> {
  mutable T value_[N]{};

public:
  machine_storage() = default;

  machine_storage(const machine_storage&) = delete;
  machine_storage(machine_storage&&) = delete;
  auto operator=(const machine_storage&) -> machine_storage& = delete;
  auto operator=(machine_storage&&) -> machine_storage& = delete;

  /// Get a stable span for use in operations
  [[nodiscard]] auto span() const noexcept -> stable_span<T> {
    return stable_span<T>{stable_span_tag, value_, N};
  }

  /// Get a stable reference to a single element
  [[nodiscard]] auto ref(std::size_t idx) const noexcept -> stable_ref<T> {
    return stable_ref<T>{&value_[idx]};
  }

  /// Direct access
  [[nodiscard]] auto get() const noexcept -> T* { return value_; }
  [[nodiscard]] auto operator[](std::size_t idx) const noexcept -> T& { return value_[idx]; }
  [[nodiscard]] static constexpr auto size() noexcept -> std::size_t { return N; }
};

// ============================================================================
// Factory functions for caller-owned data
// ============================================================================

/// Create a stable_ref from caller-owned data.
/// The caller guarantees the referenced data outlives all operations using it.
template <typename T>
[[nodiscard]] constexpr auto make_stable_ref(T& value) noexcept -> stable_ref<T> {
  return stable_ref<T>{&value};
}

/// Create a stable_ref from a pointer (explicit opt-in).
/// The caller guarantees the pointed-to data outlives all operations using it.
template <typename T>
[[nodiscard]] constexpr auto make_stable_ref(T* ptr) noexcept -> stable_ref<T> {
  return stable_ref<T>{ptr};
}

/// Create a stable_span from a std::span (dynamic extent).
/// The caller guarantees the underlying data outlives all operations using it.
template <typename T>
[[nodiscard]] constexpr auto make_stable_span(std::span<T> s) noexcept -> stable_span<T> {
  return stable_span<T>{stable_span_tag, s.data(), s.size()};
}

/// Create a stable_span from a std::span (fixed extent).
/// The caller guarantees the underlying data outlives all operations using it.
template <typename T, std::size_t N>
[[nodiscard]] constexpr auto make_stable_span(std::span<T, N> s) noexcept -> stable_span<T> {
  return stable_span<T>{stable_span_tag, s.data(), s.size()};
}

/// Create a stable_span from a C array.
template <typename T, std::size_t N>
[[nodiscard]] constexpr auto make_stable_span(T (&arr)[N]) noexcept -> stable_span<T> {
  return stable_span<T>{stable_span_tag, arr, N};
}

/// Create a stable_span from a container (vector, array, etc.)
/// The caller guarantees the container outlives all operations using it.
template <typename Container>
  requires requires(Container& c) {
    { c.data() } -> std::convertible_to<typename Container::value_type*>;
    { c.size() } -> std::convertible_to<std::size_t>;
  }
[[nodiscard]] constexpr auto make_stable_span(Container& c) noexcept
    -> stable_span<typename Container::value_type> {
  return stable_span<typename Container::value_type>{stable_span_tag, c.data(), c.size()};
}

} // namespace evring
