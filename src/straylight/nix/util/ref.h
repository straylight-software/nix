// straylight::nix::primitives::ref
//
// Non-nullable smart pointer wrapper around std::shared_ptr.
// Provides compile-time and runtime guarantees that the wrapped pointer is never null.

#pragma once

#include <compare>
#include <concepts>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace straylight::nix::util {

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
class Ref;

template <typename T, typename... Args>
[[nodiscard]] auto make_ref(Args&&... args) -> Ref<T>;

template <typename To, typename From>
  requires std::derived_from<To, From> || std::derived_from<From, To>
[[nodiscard]] auto ref_cast(const Ref<From>& from) -> Ref<To>;

// ─────────────────────────────────────────────────────────────────────────────
// Concepts for type constraints
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

template <typename From, typename To>
concept convertible_ptr = std::convertible_to<From*, To*>;

template <typename T>
concept complete_type = requires { sizeof(T); };

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Ref<T> - Non-nullable reference-counted pointer
// ─────────────────────────────────────────────────────────────────────────────

/// A non-nullable reference-counted pointer.
/// Wraps std::shared_ptr<T> with the guarantee that the pointer is never null.
/// Construction from a null pointer throws std::invalid_argument.
template <typename T>
class Ref {
public:
  using element_type = T;
  using pointer = T*;
  using reference = T&;

private:
  std::shared_ptr<T> ptr_;

  constexpr void assert_non_null() const {
    if (!ptr_) {
      throw std::invalid_argument("null pointer passed to Ref<T>");
    }
  }

public:
  // ───────────────────────────────────────────────────────────────────────────
  // Constructors
  // ───────────────────────────────────────────────────────────────────────────

  /// Construct from a shared_ptr (lvalue). Throws if null.
  explicit Ref(const std::shared_ptr<T>& p) : ptr_(p) { assert_non_null(); }

  /// Construct from a shared_ptr (rvalue). Throws if null.
  explicit Ref(std::shared_ptr<T>&& p) : ptr_(std::move(p)) { assert_non_null(); }

  /// Construct from a raw pointer. Takes ownership. Throws if null.
  explicit Ref(T* p) : ptr_(p) { assert_non_null(); }

  /// Converting constructor from compatible Ref types.
  /// Enables implicit conversion from Ref<Derived> to Ref<Base>.
  template <typename U>
    requires detail::convertible_ptr<U, T> && (!std::same_as<U, T>)
  Ref(const Ref<U>& other) : ptr_(other.get_ptr()) {}

  /// Converting constructor from compatible Ref types (rvalue).
  template <typename U>
    requires detail::convertible_ptr<U, T> && (!std::same_as<U, T>)
  Ref(Ref<U>&& other) : ptr_(std::move(other).get_ptr()) {}

  // Default copy/move operations
  Ref(const Ref&) = default;
  Ref(Ref&&) noexcept = default;
  auto operator=(const Ref&) -> Ref& = default;
  auto operator=(Ref&&) noexcept -> Ref& = default;
  ~Ref() = default;

  // ───────────────────────────────────────────────────────────────────────────
  // Accessors
  // ───────────────────────────────────────────────────────────────────────────

  /// Dereference operator. Returns reference to the managed object.
  [[nodiscard]] constexpr auto operator*() const noexcept -> T& { return *ptr_; }

  /// Arrow operator. Returns pointer to the managed object.
  [[nodiscard]] constexpr auto operator->() const noexcept -> T* { return ptr_.get(); }

  /// Get raw pointer to the managed object.
  [[nodiscard]] constexpr auto get() const noexcept -> T* { return ptr_.get(); }

  /// Get the underlying shared_ptr (lvalue).
  [[nodiscard]] auto get_ptr() const& -> std::shared_ptr<T> { return ptr_; }

  /// Get the underlying shared_ptr (rvalue, moves out).
  [[nodiscard]] auto get_ptr() && -> std::shared_ptr<T> { return std::move(ptr_); }

  /// Get the reference count.
  [[nodiscard]] auto use_count() const noexcept -> long { return ptr_.use_count(); }

  // ───────────────────────────────────────────────────────────────────────────
  // Conversions
  // ───────────────────────────────────────────────────────────────────────────

  /// Implicit conversion to shared_ptr<T>.
  /// Uses explicit object parameter (deducing this) for value category propagation.
  template <typename Self>
  [[nodiscard]] operator std::shared_ptr<T>(this Self&& self) {
    return std::forward<Self>(self).get_ptr();
  }

  /// Implicit conversion to const T&.
  [[nodiscard]] operator const T&() const noexcept { return *ptr_; }

  // ───────────────────────────────────────────────────────────────────────────
  // Casting
  // ───────────────────────────────────────────────────────────────────────────

  /// Dynamic cast to a derived type. Throws if cast fails.
  template <typename U>
    requires std::derived_from<U, T> || std::derived_from<T, U>
  [[nodiscard]] auto cast() const -> Ref<U> {
    auto casted = std::dynamic_pointer_cast<U>(ptr_);
    if (!casted) {
      throw std::invalid_argument("dynamic_cast failed in Ref::cast()");
    }
    return Ref<U>(std::move(casted));
  }

  /// Dynamic pointer cast (returns shared_ptr, may be null).
  template <typename U>
  [[nodiscard]] auto dynamic_pointer_cast() const -> std::shared_ptr<U> {
    return std::dynamic_pointer_cast<U>(ptr_);
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Comparison operators
  // ───────────────────────────────────────────────────────────────────────────

  [[nodiscard]] auto operator==(const Ref& other) const noexcept -> bool {
    return ptr_ == other.ptr_;
  }

  template <typename U>
    requires detail::convertible_ptr<U, T> || detail::convertible_ptr<T, U>
  [[nodiscard]] auto operator==(const Ref<U>& other) const noexcept -> bool {
    return ptr_ == other.get_ptr();
  }

  [[nodiscard]] auto operator<=>(const Ref& other) const noexcept -> std::strong_ordering {
    return ptr_ <=> other.ptr_;
  }

  template <typename U>
    requires detail::convertible_ptr<U, T> || detail::convertible_ptr<T, U>
  [[nodiscard]] auto operator<=>(const Ref<U>& other) const noexcept -> std::strong_ordering {
    return ptr_ <=> other.get_ptr();
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Friend declarations
  // ───────────────────────────────────────────────────────────────────────────

  template <typename U>
  friend class Ref;

  template <typename U, typename... Args>
  friend auto make_ref(Args&&... args) -> Ref<U>;

  template <typename To, typename From>
    requires std::derived_from<To, From> || std::derived_from<From, To>
  friend auto ref_cast(const Ref<From>& from) -> Ref<To>;
};

// ─────────────────────────────────────────────────────────────────────────────
// Factory function
// ─────────────────────────────────────────────────────────────────────────────

/// Create a Ref<T> by constructing T in-place with the given arguments.
/// Similar to std::make_shared but returns a Ref<T>.
template <typename T, typename... Args>
[[nodiscard]] inline auto make_ref(Args&&... args) -> Ref<T> {
  return Ref<T>(std::make_shared<T>(std::forward<Args>(args)...));
}

// ─────────────────────────────────────────────────────────────────────────────
// Dynamic cast function
// ─────────────────────────────────────────────────────────────────────────────

/// Dynamic cast a Ref<From> to Ref<To>. Throws if cast fails.
template <typename To, typename From>
  requires std::derived_from<To, From> || std::derived_from<From, To>
[[nodiscard]] inline auto ref_cast(const Ref<From>& from) -> Ref<To> {
  return from.template cast<To>();
}

} // namespace straylight::nix::util

// ─────────────────────────────────────────────────────────────────────────────
// std::hash specialization
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
struct std::hash<straylight::nix::util::Ref<T>> {
  [[nodiscard]] auto operator()(const straylight::nix::util::Ref<T>& ref) const noexcept
      -> std::size_t {
    return std::hash<std::shared_ptr<T>>{}(ref.get_ptr());
  }
};
