#ifndef NIX_UTIL_CALLBACK_H
#define NIX_UTIL_CALLBACK_H
///@file

#include <atomic>
#include <cassert>
#include <exception>
#include <functional>
#include <future>

namespace nix {

/**
 * A callback is a wrapper around a lambda that accepts a valid of
 * type T or an exception. (We abuse std::future<T> to pass the value or
 * exception.)
 */
template <typename T>
class callback {
  std::function<void(std::future<T>)> fun_;
  std::atomic_flag done_ = ATOMIC_FLAG_INIT;

public:
  callback(std::function<void(std::future<T>)> func) : fun_(func) {}

  // NOTE: std::function is noexcept move-constructible since C++20.
  callback(callback&& other) noexcept(std::is_nothrow_move_constructible_v<decltype(fun_)>)
      : fun_(std::move(other.fun_)) {
    auto prev = other.done_.test_and_set();
    if (prev) {
      done_.test_and_set();
    }
  }

  callback(const callback&) = delete;
  auto operator=(const callback&) -> callback& = delete;
  auto operator=(callback&&) -> callback& = delete;
  ~callback() = default;

  void operator()(T&& val) noexcept {
    auto prev = done_.test_and_set();
    assert(!prev);
    std::promise<T> promise;
    promise.set_value(std::move(val));
    fun_(promise.get_future());
  }

  void rethrow(const std::exception_ptr& exc = std::current_exception()) noexcept {
    auto prev = done_.test_and_set();
    assert(!prev);
    std::promise<T> promise;
    promise.set_exception(exc);
    fun_(promise.get_future());
  }
};

// Compatibility alias for legacy code during naming convention migration
template <typename T>
using Callback = callback<T>;

} // namespace nix

#endif // NIX_UTIL_CALLBACK_H
