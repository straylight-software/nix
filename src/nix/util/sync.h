#pragma once
///@file

#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>

#include "nix/util/error.h"

namespace nix {

/**
 * This template class ensures synchronized access to a value of type
 * T. It is used as follows:
 *
 *   struct Data { int x; ... };
 *
 *   sync_t<Data> data;
 *
 *   {
 *     auto data_(data.lock());
 *     data_->x = 123;
 *   }
 *
 * Here, "data" is automatically unlocked when "data_" goes out of
 * scope.
 */
template <class T, class M, class WL, class RL>
class sync_base_t {
private:
  M mutex;
  T data;

public:
  using element_type = T;

  sync_base_t() {}

  sync_base_t(const T& data) : data(data) {}

  sync_base_t(T&& data) noexcept : data(std::move(data)) {}

  sync_base_t(sync_base_t&& other) noexcept : data(std::move(*other.lock())) {}

  template <class L>
  class lock_t {
  protected:
    sync_base_t* s;
    L lk;
    friend sync_base_t;

    lock_t(sync_base_t* s) : s(s), lk(s->mutex) {}

  public:
    lock_t(lock_t&& l) : s(l.s) { unreachable(); }

    lock_t(const lock_t& l) = delete;

    ~lock_t() {}

    void wait(std::condition_variable& cv) {
      assert(s);
      cv.wait(lk);
    }

    template <class Rep, class Period>
    std::cv_status wait_for(std::condition_variable& cv,
                            const std::chrono::duration<Rep, Period>& duration) {
      assert(s);
      return cv.wait_for(lk, duration);
    }

    template <class Rep, class Period, class Predicate>
    bool wait_for(std::condition_variable& cv, const std::chrono::duration<Rep, Period>& duration,
                  Predicate pred) {
      assert(s);
      return cv.wait_for(lk, duration, pred);
    }

    template <class Clock, class Duration>
    std::cv_status wait_until(std::condition_variable& cv,
                              const std::chrono::time_point<Clock, Duration>& duration) {
      assert(s);
      return cv.wait_until(lk, duration);
    }
  };

  struct write_lock_t : lock_t<WL> {
    T* operator->() { return &write_lock_t::s->data; }

    T& operator*() { return write_lock_t::s->data; }
  };

  /**
   * Acquire write (exclusive) access to the inner value.
   */
  write_lock_t lock() { return write_lock_t(this); }

  struct read_lock_t : lock_t<RL> {
    const T* operator->() { return &read_lock_t::s->data; }

    const T& operator*() { return read_lock_t::s->data; }
  };

  /**
   * Acquire read access to the inner value. When using
   * `std::shared_mutex`, this will use a shared lock.
   */
  read_lock_t read_lock() const { return read_lock_t(const_cast<sync_base_t*>(this)); }
};

template <class T>
using sync_t =
    sync_base_t<T, std::mutex, std::unique_lock<std::mutex>, std::unique_lock<std::mutex>>;

template <class T>
using shared_sync_t = sync_base_t<T, std::shared_mutex, std::unique_lock<std::shared_mutex>,
                                  std::shared_lock<std::shared_mutex>>;

} // namespace nix
