#pragma once
///@file

#include <memory>
#include <stdexcept>

namespace nix {

/**
 * A simple non-nullable reference-counted pointer. Actually a wrapper
 * around std::shared_ptr that prevents null constructions.
 */
template <typename T>
class ref {
private:
  std::shared_ptr<T> p_{};

  void assert_non_null() {
    if (!p_)
      throw std::invalid_argument("null pointer cast to ref");
  }

public:
  using element_type = T;

  explicit ref(const std::shared_ptr<T>& p) : p_(p) { assert_non_null(); }

  explicit ref(std::shared_ptr<T>&& p) : p_(std::move(p)) { assert_non_null(); }

  explicit ref(T* p) : p_(p) { assert_non_null(); }

  auto operator->() const -> T* { return &*p_; }

  auto operator*() const -> T& { return *p_; }

  [[nodiscard]] std::shared_ptr<T> get_ptr() const& { return p_; }

  std::shared_ptr<T> get_ptr() && { return std::move(p_); }

  /**
   * Convenience to avoid explicit `get_ptr()` call in some cases.
   */
  operator std::shared_ptr<T>(this auto&& self) {
    return std::forward<decltype(self)>(self).get_ptr();
  }

  template <typename T2>
  ref<T2> cast() const {
    return ref<T2>(std::dynamic_pointer_cast<T2>(p_));
  }

  template <typename T2>
  std::shared_ptr<T2> dynamic_pointer_cast() const {
    return std::dynamic_pointer_cast<T2>(p_);
  }

  template <typename T2>
  operator ref<T2>() const {
    return ref<T2>((std::shared_ptr<T2>)p_);
  }

  bool operator==(const ref<T>& other) const { return p_ == other.p_; }

  bool operator!=(const ref<T>& other) const { return p_ != other.p_; }

  auto operator<=>(const ref<T>& other) const { return p_ <=> other.p_; }

private:
  template <typename T2, typename... Args>
  friend ref<T2> make_ref(Args&&... args);
};

template <typename T, typename... Args>
inline auto make_ref(Args&&... args) -> ref<T> {
  auto p = std::make_shared<T>(std::forward<Args>(args)...);
  return ref<T>(p);
}

} // namespace nix
