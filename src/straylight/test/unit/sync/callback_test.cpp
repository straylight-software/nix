// straylight::nix::sync::callback tests
//
// Unit tests for the one-shot callback primitive.

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <straylight/nix/sync/callback.h>

namespace callback = straylight::nix::sync;

// ─────────────────────────────────────────────────────────────────────────────
// Basic functionality tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("callback basic value delivery", "[callback]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb(42);

  REQUIRE(future.get() == 42);
}

TEST_CASE("callback string value delivery", "[callback]") {
  callback::Callback<std::string> cb;
  auto future = cb.get_future();

  cb("hello world");

  REQUIRE(future.get() == "hello world");
}

TEST_CASE("callback void type", "[callback]") {
  callback::Callback<void> cb;
  auto future = cb.get_future();

  cb();

  // Should not throw
  REQUIRE_NOTHROW(future.get());
}

TEST_CASE("callback exception delivery", "[callback]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb.rethrow(std::make_exception_ptr(std::runtime_error("test error")));

  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("callback void exception delivery", "[callback]") {
  callback::Callback<void> cb;
  auto future = cb.get_future();

  cb.rethrow(std::make_exception_ptr(std::runtime_error("void error")));

  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// One-shot semantics tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("callback throws on double invocation with value", "[callback][oneshot]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb(42);

  REQUIRE_THROWS_AS(cb(100), callback::callback_already_invoked);
  REQUIRE(future.get() == 42); // First value wins
}

TEST_CASE("callback throws on double invocation with exception", "[callback][oneshot]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb.rethrow(std::make_exception_ptr(std::runtime_error("first")));

  REQUIRE_THROWS_AS(cb.rethrow(std::make_exception_ptr(std::runtime_error("second"))),
                    callback::callback_already_invoked);
}

TEST_CASE("callback throws on value after exception", "[callback][oneshot]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb.rethrow(std::make_exception_ptr(std::runtime_error("error")));

  REQUIRE_THROWS_AS(cb(42), callback::callback_already_invoked);
}

TEST_CASE("callback throws on exception after value", "[callback][oneshot]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb(42);

  REQUIRE_THROWS_AS(cb.rethrow(std::make_exception_ptr(std::runtime_error("error"))),
                    callback::callback_already_invoked);
}

TEST_CASE("callback void throws on double invocation", "[callback][oneshot]") {
  callback::Callback<void> cb;
  auto future = cb.get_future();

  cb();

  REQUIRE_THROWS_AS(cb(), callback::callback_already_invoked);
}

// ─────────────────────────────────────────────────────────────────────────────
// Invoked state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("callback invoked() returns correct state", "[callback][state]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  REQUIRE_FALSE(cb.invoked());

  cb(42);

  REQUIRE(cb.invoked());
}

TEST_CASE("callback void invoked() returns correct state", "[callback][state]") {
  callback::Callback<void> cb;
  auto future = cb.get_future();

  REQUIRE_FALSE(cb.invoked());

  cb();

  REQUIRE(cb.invoked());
}

TEST_CASE("callback invoked() true after rethrow", "[callback][state]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb.rethrow(std::make_exception_ptr(std::runtime_error("error")));

  REQUIRE(cb.invoked());
}

// ─────────────────────────────────────────────────────────────────────────────
// Move semantics tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("callback is move constructible", "[callback][move]") {
  callback::Callback<int> cb1;
  auto future = cb1.get_future();

  callback::Callback<int> cb2 = std::move(cb1);

  cb2(42);

  REQUIRE(future.get() == 42);
}

TEST_CASE("callback is move assignable", "[callback][move]") {
  callback::Callback<int> cb1;
  auto future = cb1.get_future();

  callback::Callback<int> cb2;
  cb2 = std::move(cb1);

  cb2(42);

  REQUIRE(future.get() == 42);
}

TEST_CASE("moved-from callback is marked as invoked", "[callback][move]") {
  callback::Callback<int> cb1;
  [[maybe_unused]] auto future = cb1.get_future();

  callback::Callback<int> cb2 = std::move(cb1);

  // The moved-from callback should be in an invoked state to prevent use
  REQUIRE(cb1.invoked());
  REQUIRE_FALSE(cb2.invoked());
}

TEST_CASE("callback void is move constructible", "[callback][move]") {
  callback::Callback<void> cb1;
  auto future = cb1.get_future();

  callback::Callback<void> cb2 = std::move(cb1);

  cb2();

  REQUIRE_NOTHROW(future.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread safety tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("callback thread-safe value delivery", "[callback][thread]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  std::thread producer([&cb]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    cb(42);
  });

  // Consumer waits on future
  int result = future.get();
  producer.join();

  REQUIRE(result == 42);
}

TEST_CASE("callback thread-safe exception delivery", "[callback][thread]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  std::thread producer([&cb]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    cb.rethrow(std::make_exception_ptr(std::runtime_error("async error")));
  });

  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
  producer.join();
}

TEST_CASE("callback concurrent invocation only one succeeds", "[callback][thread]") {
  constexpr int kNumThreads = 10;

  callback::Callback<int> cb;
  auto future = cb.get_future();

  std::atomic<int> success_count{0};
  std::atomic<int> failure_count{0};
  std::vector<std::thread> threads;

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&cb, &success_count, &failure_count, i]() {
      try {
        cb(i);
        success_count.fetch_add(1);
      } catch (const callback::callback_already_invoked&) {
        failure_count.fetch_add(1);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  REQUIRE(success_count.load() == 1);
  REQUIRE(failure_count.load() == kNumThreads - 1);

  // The future should have exactly one value
  [[maybe_unused]] int result = future.get();
}

TEST_CASE("callback void concurrent invocation only one succeeds", "[callback][thread]") {
  constexpr int kNumThreads = 10;

  callback::Callback<void> cb;
  auto future = cb.get_future();

  std::atomic<int> success_count{0};
  std::atomic<int> failure_count{0};
  std::vector<std::thread> threads;

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&cb, &success_count, &failure_count]() {
      try {
        cb();
        success_count.fetch_add(1);
      } catch (const callback::callback_already_invoked&) {
        failure_count.fetch_add(1);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  REQUIRE(success_count.load() == 1);
  REQUIRE(failure_count.load() == kNumThreads - 1);

  REQUIRE_NOTHROW(future.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// Complex type tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("callback with vector", "[callback][types]") {
  callback::Callback<std::vector<int>> cb;
  auto future = cb.get_future();

  cb(std::vector<int>{1, 2, 3, 4, 5});

  auto result = future.get();
  REQUIRE(result == std::vector<int>{1, 2, 3, 4, 5});
}

TEST_CASE("callback with move-only type", "[callback][types]") {
  callback::Callback<std::unique_ptr<int>> cb;
  auto future = cb.get_future();

  cb(std::make_unique<int>(42));

  auto result = future.get();
  REQUIRE(*result == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// Custom exception tests
// ─────────────────────────────────────────────────────────────────────────────

struct CustomException : public std::exception {
  explicit CustomException(std::string msg) : msg_(std::move(msg)) {}
  [[nodiscard]] const char* what() const noexcept override { return msg_.c_str(); }

  std::string msg_;
};

TEST_CASE("callback custom exception type", "[callback][exception]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb.rethrow(std::make_exception_ptr(CustomException("custom error")));

  try {
    future.get();
    FAIL("Expected exception");
  } catch (const CustomException& e) {
    REQUIRE(std::string(e.what()) == "custom error");
  }
}

TEST_CASE("callback_already_invoked exception message", "[callback][exception]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb(42);

  try {
    cb(100);
    FAIL("Expected callback_already_invoked");
  } catch (const callback::callback_already_invoked& e) {
    REQUIRE(std::string(e.what()).find("already") != std::string::npos);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge case tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("callback with zero value", "[callback][edge]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  cb(0);

  REQUIRE(future.get() == 0);
}

TEST_CASE("callback with empty string", "[callback][edge]") {
  callback::Callback<std::string> cb;
  auto future = cb.get_future();

  cb("");

  REQUIRE(future.get().empty());
}

TEST_CASE("callback with nullptr", "[callback][edge]") {
  callback::Callback<int*> cb;
  auto future = cb.get_future();

  cb(nullptr);

  REQUIRE(future.get() == nullptr);
}

TEST_CASE("callback rethrow with nullptr exception_ptr", "[callback][edge]") {
  callback::Callback<int> cb;
  auto future = cb.get_future();

  // Passing nullptr to set_exception causes undefined behavior in std::promise.
  // On libstdc++, this may terminate the program or behave unexpectedly.
  // We test that the callback is at least marked as invoked.
  std::exception_ptr null_exc;

  // This is UB per the standard, but we document that it at least marks invoked
  cb.rethrow(null_exc);
  REQUIRE(cb.invoked());

  // Note: Calling future.get() after this is undefined behavior.
  // Some implementations may throw, others may return garbage.
  // We intentionally don't call future.get() to avoid UB.
}
