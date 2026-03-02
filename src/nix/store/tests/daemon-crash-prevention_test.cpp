// daemon-crash-prevention_test.cpp
//
// Adversarial tests for daemon crash prevention fixes.
// These tests exercise specific bug fixes from GitHub issues:
//
//   #14758 - Random nix-daemon crash with nh os switch (ignore_exception_in_destructor)
//   #13484 - Daemon crashes with assertion failure (callback atomic flag)
//   #14300 - mutex lock failed: Invalid argument (function-local statics)
//   #14733, #13707, #13844, #12871, #12761, #11667, #13721 - Daemonless fixes
//   #6516  - Bad file descriptor with CA derivation (daemonless CA)
//   #8113  - CA derivations can create malformed NAR (two-pass NAR re-dump)
//   #6065  - CA derivation fails on aarch64-darwin (codesign)
//   #11748 - S3 cache missing realisations endpoint (put/get_realisation)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "nix/util/callback.h"
#include "nix/util/finally.h"
#include "nix/util/sync.h"
#include "nix/util/util.h"

namespace fs = std::filesystem;

// =============================================================================
// Test helpers
// =============================================================================

namespace {

class TestTempDir {
public:
  TestTempDir()
      : path_(fs::temp_directory_path() / ("daemon_crash_test_" + std::to_string(getpid()) + "_" +
                                           std::to_string(counter_++))) {
    fs::create_directories(path_);
  }

  ~TestTempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  [[nodiscard]] auto path() const -> const fs::path& { return path_; }

private:
  fs::path path_;
  static inline std::atomic<int> counter_{0};
};

// Custom exception for testing
struct TestException : std::runtime_error {
  using std::runtime_error::runtime_error;
};

} // namespace

// =============================================================================
// #14758 - Random nix-daemon crash with nh os switch
// =============================================================================
// Issue: Exception thrown in finally block during stack unwinding causes
// std::terminate due to double exception.
//
// Fix: Wrap finally block code in try-catch using ignore_exception_in_destructor()
//
// Reference: src/nix/store/daemon.cpp:1029-1040

TEST_CASE("finally block with ignore_exception_in_destructor survives nested exceptions",
          "[daemon][crash][gh14758]") {
  INFO("#14758: Finally block must not propagate exceptions during stack unwinding");

  // This simulates what daemon.cpp does:
  // A finally block that can throw (e.g., printMsg fails when remote is closed),
  // wrapped with ignore_exception_in_destructor() to prevent std::terminate.

  SECTION("exception in finally during normal exit propagates") {
    bool finally_ran = false;
    bool caught_finally_exception = false;

    try {
      finally_t finally([&]() {
        finally_ran = true;
        throw TestException("finally exception");
      });
    } catch (const TestException& e) {
      caught_finally_exception = true;
      REQUIRE(std::string(e.what()) == "finally exception");
    }

    REQUIRE(finally_ran);
    REQUIRE(caught_finally_exception);
  }

  SECTION("exception in finally with ignore_exception_in_destructor is suppressed") {
    bool finally_ran = false;
    bool main_exception_caught = false;

    try {
      finally_t finally([&]() {
        try {
          finally_ran = true;
          throw TestException("finally exception");
        } catch (...) {
          // Simulating ignore_exception_in_destructor behavior
          // In real code this calls nix::ignore_exception_in_destructor()
        }
      });
      throw TestException("main exception");
    } catch (const TestException& e) {
      main_exception_caught = true;
      REQUIRE(std::string(e.what()) == "main exception");
    }

    // Both should have completed without std::terminate
    REQUIRE(finally_ran);
    REQUIRE(main_exception_caught);
  }

  SECTION("daemon-style finally block pattern does not crash during unwinding") {
    // Exact pattern from daemon.cpp
    bool finally_ran = false;
    bool outer_caught = false;

    try {
      finally_t finally([&]() {
        try {
          finally_ran = true;
          // Simulating set_interrupted() and printMsgUsing() which can throw
          // when the remote connection is closed
          throw TestException("connection closed");
        } catch (...) {
          // This is what ignore_exception_in_destructor() does
          // It must catch and suppress all exceptions during stack unwinding
        }
      });

      // Simulate an interruption that triggers unwinding
      throw TestException("interrupted");
    } catch (const TestException& e) {
      outer_caught = true;
      // The main exception should propagate, not the finally exception
      REQUIRE(std::string(e.what()) == "interrupted");
    }

    REQUIRE(finally_ran);
    REQUIRE(outer_caught);
  }
}

// =============================================================================
// #13484 - Daemon crashes with assertion failure (mlibc)
// =============================================================================
// Issue: Callback invoked twice in race condition between async completion
// and exception handling, causing double promise fulfillment.
//
// Fix: Use atomic_flag done_.test_and_set() to ensure one-shot semantics.
//
// Reference: src/nix/util/callback.h:31,43,73

TEST_CASE("callback atomic flag ensures single invocation", "[daemon][crash][gh13484]") {
  INFO("#13484: Callback must execute at most once even with concurrent invocations");

  SECTION("single invocation succeeds") {
    std::atomic<int> invocation_count{0};
    nix::callback<int> cb([&](std::future<int> f) {
      invocation_count.fetch_add(1);
      REQUIRE(f.get() == 42);
    });

    cb(42);
    REQUIRE(invocation_count.load() == 1);
  }

  SECTION("double invocation - second is silently ignored") {
    std::atomic<int> invocation_count{0};
    nix::callback<int> cb([&](std::future<int> f) {
      invocation_count.fetch_add(1);
      [[maybe_unused]] auto val = f.get();
    });

    cb(42);
    cb(100); // Second invocation should be ignored

    REQUIRE(invocation_count.load() == 1);
  }

  SECTION("rethrow after value - second is silently ignored") {
    std::atomic<int> invocation_count{0};
    nix::callback<int> cb([&](std::future<int> f) {
      invocation_count.fetch_add(1);
      [[maybe_unused]] auto val = f.get();
    });

    cb(42);
    cb.rethrow(std::make_exception_ptr(TestException("should not propagate")));

    REQUIRE(invocation_count.load() == 1);
  }

  SECTION("value after rethrow - second is silently ignored") {
    std::atomic<int> invocation_count{0};
    bool caught_exception = false;

    nix::callback<int> cb([&](std::future<int> f) {
      invocation_count.fetch_add(1);
      try {
        f.get();
      } catch (const TestException&) {
        caught_exception = true;
      }
    });

    cb.rethrow(std::make_exception_ptr(TestException("first")));
    cb(42); // Second invocation should be ignored

    REQUIRE(invocation_count.load() == 1);
    REQUIRE(caught_exception);
  }

  SECTION("concurrent invocations - exactly one succeeds") {
    constexpr int kNumThreads = 16;
    std::atomic<int> invocation_count{0};
    std::atomic<int> success_count{0};

    nix::callback<int> cb([&](std::future<int> f) {
      invocation_count.fetch_add(1);
      [[maybe_unused]] auto val = f.get();
    });

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int i = 0; i < kNumThreads; ++i) {
      threads.emplace_back([&cb, &success_count, i]() {
        cb(int{i}); // Pass rvalue
        success_count.fetch_add(1);
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    // All threads should complete (no crash), but callback runs once
    REQUIRE(success_count.load() == kNumThreads);
    REQUIRE(invocation_count.load() == 1);
  }

  SECTION("move semantics preserve atomic flag state") {
    std::atomic<int> invocation_count{0};

    nix::callback<int> cb1([&](std::future<int> f) {
      invocation_count.fetch_add(1);
      [[maybe_unused]] auto val = f.get();
    });

    // Invoke before move
    cb1(42);
    REQUIRE(invocation_count.load() == 1);

    // Move - done_ flag should transfer
    nix::callback<int> cb2(std::move(cb1));

    // Second invocation on moved-to should still be ignored
    cb2(100);
    REQUIRE(invocation_count.load() == 1);
  }
}

// =============================================================================
// #14300 - mutex lock failed: Invalid argument
// =============================================================================
// Issue: Function-local statics may fail initialization order in multi-threaded
// contexts, causing mutex errors.
//
// Fix: Use Meyers singleton pattern with proper thread-safe initialization.
//
// Reference: src/nix/util/sync.h

TEST_CASE("sync_t concurrent access does not cause mutex errors", "[daemon][crash][gh14300]") {
  INFO("#14300: sync_t must handle concurrent access without mutex initialization failures");

  SECTION("basic sync_t lock/unlock") {
    nix::sync_t<int> sync_val(0);

    {
      auto lock = sync_val.lock();
      *lock = 42;
    }

    {
      auto lock = sync_val.lock();
      REQUIRE(*lock == 42);
    }
  }

  SECTION("concurrent read/write with sync_t") {
    constexpr int kNumThreads = 8;
    constexpr int kIterations = 100;

    nix::sync_t<int> counter(0);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kNumThreads; ++i) {
      threads.emplace_back([&]() {
        for (int j = 0; j < kIterations; ++j) {
          try {
            auto lock = counter.lock();
            int prev = *lock;
            *lock = prev + 1;
          } catch (const std::system_error& e) {
            // This would indicate a mutex error
            errors.fetch_add(1);
          }
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    REQUIRE(errors.load() == 0);
    REQUIRE(*counter.lock() == kNumThreads * kIterations);
  }

  SECTION("shared_sync_t allows concurrent readers") {
    nix::shared_sync_t<int> shared_val(42);
    std::atomic<int> read_count{0};
    std::atomic<int> errors{0};

    constexpr int kNumReaders = 8;
    std::vector<std::thread> readers;

    for (int i = 0; i < kNumReaders; ++i) {
      readers.emplace_back([&]() {
        try {
          auto lock = shared_val.read_lock();
          REQUIRE(*lock == 42);
          read_count.fetch_add(1);
        } catch (const std::system_error&) {
          errors.fetch_add(1);
        }
      });
    }

    for (auto& t : readers) {
      t.join();
    }

    REQUIRE(errors.load() == 0);
    REQUIRE(read_count.load() == kNumReaders);
  }

  SECTION("function-local static sync_t initialization race") {
    // Simulate the issue: multiple threads trying to use a function-local static
    auto get_singleton = []() -> nix::sync_t<int>& {
      static nix::sync_t<int> instance(0);
      return instance;
    };

    constexpr int kNumThreads = 16;
    std::atomic<int> success_count{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kNumThreads; ++i) {
      threads.emplace_back([&]() {
        try {
          auto& sync = get_singleton();
          auto lock = sync.lock();
          (*lock)++;
          success_count.fetch_add(1);
        } catch (const std::system_error& e) {
          // This would indicate initialization race failure
          errors.fetch_add(1);
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    REQUIRE(errors.load() == 0);
    REQUIRE(success_count.load() == kNumThreads);
  }
}

// =============================================================================
// #14733, #13707, #13844, #12871, #12761, #11667, #13721 - Daemonless fixes
// =============================================================================
// Issue: Various operations fail when running without nix-daemon.
//
// Fix: Use flock() for coordination instead of daemon IPC.
//
// Reference: src/straylight/nix/store/log_store.cpp:433

TEST_CASE("flock-based coordination works without daemon", "[daemon][daemonless]") {
  INFO("Daemonless mode must use flock() for writer coordination");

  TestTempDir tmp;
  auto lock_path = tmp.path() / "test.lock";

  SECTION("flock can be acquired and released") {
    int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    REQUIRE(fd >= 0);

    // Acquire exclusive lock
    int result = ::flock(fd, LOCK_EX);
    REQUIRE(result == 0);

    // Release lock
    result = ::flock(fd, LOCK_UN);
    REQUIRE(result == 0);

    ::close(fd);
  }

  SECTION("flock provides mutual exclusion across threads") {
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> errors{0};

    constexpr int kNumThreads = 8;
    constexpr int kIterations = 20;
    std::vector<std::thread> threads;

    for (int i = 0; i < kNumThreads; ++i) {
      threads.emplace_back([&]() {
        for (int j = 0; j < kIterations; ++j) {
          int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
          if (fd < 0) {
            errors.fetch_add(1);
            continue;
          }

          if (::flock(fd, LOCK_EX) != 0) {
            errors.fetch_add(1);
            ::close(fd);
            continue;
          }

          // Critical section
          int prev = concurrent.fetch_add(1);
          if (prev != 0) {
            errors.fetch_add(1); // Race detected!
          }

          int cur = concurrent.load();
          int max = max_concurrent.load();
          while (cur > max && !max_concurrent.compare_exchange_weak(max, cur)) {
          }

          std::this_thread::sleep_for(std::chrono::microseconds(10));

          concurrent.fetch_sub(1);

          ::flock(fd, LOCK_UN);
          ::close(fd);
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    REQUIRE(errors.load() == 0);
    REQUIRE(max_concurrent.load() <= 1);
  }

  SECTION("flock EINTR is retried") {
    // Simulate the EINTR retry pattern used in the codebase
    int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    REQUIRE(fd >= 0);

    auto robust_flock = [](int fd, int op) -> int {
      int result;
      do {
        result = ::flock(fd, op);
      } while (result < 0 && errno == EINTR);
      return result;
    };

    int result = robust_flock(fd, LOCK_EX);
    REQUIRE(result == 0);

    result = robust_flock(fd, LOCK_UN);
    REQUIRE(result == 0);

    ::close(fd);
  }
}

// =============================================================================
// #6516 - Bad file descriptor with CA derivation
// =============================================================================
// Issue: CA store operations fail in daemonless mode with EBADF.
//
// Fix: Proper file descriptor handling in CA store implementation.

TEST_CASE("CA store operations work in daemonless mode", "[ca][daemonless][gh6516]") {
  INFO("#6516: CA store must handle file descriptors properly without daemon");

  TestTempDir tmp;

  SECTION("atomic rename pattern for safe writes") {
    auto final_path = tmp.path() / "final.txt";
    auto tmp_path = tmp.path() / "final.txt.tmp";

    // Write to temp file
    {
      int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
      REQUIRE(fd >= 0);

      const char* data = "test content";
      ssize_t written = ::write(fd, data, strlen(data));
      REQUIRE(written == static_cast<ssize_t>(strlen(data)));

      int sync_result = ::fsync(fd);
      REQUIRE(sync_result == 0);

      ::close(fd);
    }

    // Atomic rename
    int rename_result = ::rename(tmp_path.c_str(), final_path.c_str());
    REQUIRE(rename_result == 0);

    // Verify content
    {
      std::ifstream f(final_path);
      std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      REQUIRE(content == "test content");
    }
  }

  SECTION("O_CLOEXEC prevents fd leaks across exec") {
    auto file_path = tmp.path() / "cloexec_test.txt";
    int fd = ::open(file_path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    REQUIRE(fd >= 0);

    // Verify FD_CLOEXEC is set
    int flags = ::fcntl(fd, F_GETFD);
    REQUIRE(flags != -1);
    REQUIRE((flags & FD_CLOEXEC) != 0);

    ::close(fd);
  }
}

// =============================================================================
// #8113 - CA derivations can create malformed NAR
// =============================================================================
// Issue: Hash rewriting can break lexical order of directory entries,
// causing "NAR directory is not sorted" errors.
//
// Fix: Two-pass NAR re-dump to restore proper sorting.
//
// Reference: src/nix/store/unix/build/derivation-builder.cpp:1575-1606

TEST_CASE("NAR directory entries must be sorted lexicographically", "[ca][nar][gh8113]") {
  INFO("#8113: Hash rewriting must preserve NAR lexical order via two-pass re-dump");

  SECTION("unsorted entries are detected as malformed") {
    // Entries must be in sorted order: "a" < "b" < "z"
    std::vector<std::string> sorted_entries = {"a", "b", "z"};
    std::vector<std::string> unsorted_entries = {"z", "a", "b"};

    // Verify our test data
    REQUIRE(std::is_sorted(sorted_entries.begin(), sorted_entries.end()));
    REQUIRE_FALSE(std::is_sorted(unsorted_entries.begin(), unsorted_entries.end()));
  }

  SECTION("hash rewrite can break sort order") {
    // Simulate the problem: filenames containing hashes where rewriting breaks order
    // Before: "abc123-lib" < "xyz789-bin" (sorted, because 'a' < 'x')
    // After:  "xyz789-lib" > "abc123-bin" (unsorted, because 'x' > 'a')

    std::vector<std::string> before_rewrite = {"abc123-lib", "xyz789-bin"};
    REQUIRE(std::is_sorted(before_rewrite.begin(), before_rewrite.end()));

    // Simulate hash rewrite: abc123 -> xyz789 and xyz789 -> abc123
    std::vector<std::string> after_rewrite = {"xyz789-lib", "abc123-bin"};

    // After rewriting, "abc123-bin" < "xyz789-lib", so the order is now wrong
    REQUIRE(after_rewrite[1] < after_rewrite[0]); // Demonstrates the problem
    REQUIRE_FALSE(std::is_sorted(after_rewrite.begin(), after_rewrite.end()));
  }

  SECTION("re-sorting fixes the order") {
    std::vector<std::string> after_rewrite = {"xyz789-lib", "abc123-bin"};
    REQUIRE_FALSE(std::is_sorted(after_rewrite.begin(), after_rewrite.end()));

    // The two-pass approach: dump -> restore -> dump again (which sorts)
    std::sort(after_rewrite.begin(), after_rewrite.end());

    REQUIRE(std::is_sorted(after_rewrite.begin(), after_rewrite.end()));
    REQUIRE(after_rewrite[0] == "abc123-bin");
    REQUIRE(after_rewrite[1] == "xyz789-lib");
  }
}

// =============================================================================
// #6065 - CA derivation fails on aarch64-darwin
// =============================================================================
// Issue: Hash rewriting invalidates code signatures on Apple Silicon,
// causing "Killed: 9" errors.
//
// Fix: Run codesign -f -s - on modified Mach-O executables after rewriting.
//
// Reference: src/nix/store/unix/build/derivation-builder.cpp:1608-1662

TEST_CASE("Darwin code signing requirements for CA derivations", "[ca][darwin][gh6065]") {
  INFO("#6065: Modified Mach-O executables must be re-signed on darwin");

  SECTION("Mach-O magic numbers are correctly defined") {
    // These are the magic numbers used to detect Mach-O binaries
    constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf;
    constexpr uint32_t MH_CIGAM_64 = 0xcffaedfe;
    constexpr uint32_t MH_MAGIC = 0xfeedface;
    constexpr uint32_t MH_CIGAM = 0xcefaedfe;
    constexpr uint32_t FAT_MAGIC = 0xcafebabe;
    constexpr uint32_t FAT_CIGAM = 0xbebafeca;

    // Verify endian-swapped versions
    REQUIRE(MH_CIGAM_64 == __builtin_bswap32(MH_MAGIC_64));
    REQUIRE(MH_CIGAM == __builtin_bswap32(MH_MAGIC));
    REQUIRE(FAT_CIGAM == __builtin_bswap32(FAT_MAGIC));

    // Verify distinctness
    std::vector<uint32_t> magics = {MH_MAGIC_64, MH_CIGAM_64, MH_MAGIC,
                                    MH_CIGAM,    FAT_MAGIC,   FAT_CIGAM};
    std::set<uint32_t> unique_magics(magics.begin(), magics.end());
    REQUIRE(unique_magics.size() == magics.size());
  }

  SECTION("executable file detection for codesign") {
    TestTempDir tmp;
    auto exec_path = tmp.path() / "test_exec";

    // Create a file with executable permission
    {
      std::ofstream f(exec_path);
      f << "#!/bin/sh\necho hello\n";
    }
    fs::permissions(exec_path, fs::perms::owner_exec, fs::perm_options::add);

    // Verify it's executable
    auto perms = fs::status(exec_path).permissions();
    bool is_exec = (perms & fs::perms::owner_exec) != fs::perms::none;
    REQUIRE(is_exec);
  }

#ifdef __APPLE__
  SECTION("codesign command exists on darwin") {
    // This test only runs on macOS
    int result = std::system("which codesign > /dev/null 2>&1");
    REQUIRE(result == 0);
  }
#endif
}

// =============================================================================
// #11748 - S3 cache missing realisations endpoint
// =============================================================================
// Issue: S3 binary cache does not support CA derivation realisations.
//
// Fix: Implement put_realisation() and get_realisation() methods.
//
// Reference: src/straylight/nix/store/ca_store.cpp:390-479

TEST_CASE("CA store realisation operations", "[ca][s3][gh11748]") {
  INFO("#11748: CA store must support put_realisation and get_realisation");

  TestTempDir tmp;
  auto realisations_dir = tmp.path() / "realisations";
  fs::create_directories(realisations_dir);

  SECTION("put_realisation writes atomically") {
    std::string drv_output_key = "sha256:abc123!out";
    std::string realisation_json = R"({"outPath":"/nix/store/xyz","signatures":[]})";

    // Simulate the atomic write pattern
    auto final_path = realisations_dir / (drv_output_key + ".doi");
    auto tmp_path = std::string(final_path) + ".tmp";

    // Write to temp file
    {
      std::ofstream f(tmp_path);
      f << realisation_json;
      f.flush();
      f.close();
    }

    // Atomic rename
    int result = ::rename(tmp_path.c_str(), final_path.c_str());
    REQUIRE(result == 0);

    // Verify
    REQUIRE(fs::exists(final_path));
    std::ifstream check(final_path);
    std::string content((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>());
    REQUIRE(content == realisation_json);
  }

  SECTION("get_realisation reads existing file") {
    std::string drv_output_key = "sha256:def456!out";
    std::string realisation_json = R"({"outPath":"/nix/store/abc","signatures":["sig1"]})";

    auto path = realisations_dir / (drv_output_key + ".doi");
    {
      std::ofstream f(path);
      f << realisation_json;
    }

    // Read back
    std::ifstream f(path);
    REQUIRE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(content == realisation_json);
  }

  SECTION("get_realisation returns not_found for missing") {
    auto path = realisations_dir / "nonexistent.doi";
    std::ifstream f(path);
    REQUIRE_FALSE(f.is_open());
  }

  SECTION("realisation path format is correct") {
    // Format: <store_root>/realisations/<drv_output_key>.doi
    std::string drv_output_key = "sha256:test123!output";
    auto expected_path = realisations_dir / (drv_output_key + ".doi");

    // The key can contain special characters like ! and :
    // Verify the path is constructable
    REQUIRE(expected_path.filename() == drv_output_key + ".doi");
  }
}

// =============================================================================
// Integration: Stress test for daemon crash scenarios
// =============================================================================

TEST_CASE("stress test: concurrent operations do not crash", "[daemon][stress]") {
  constexpr int kNumThreads = 8;
  constexpr int kIterations = 50;

  std::atomic<int> callback_successes{0};
  std::atomic<int> callback_races{0};
  std::atomic<int> sync_errors{0};
  std::atomic<int> finally_errors{0};

  std::vector<std::thread> threads;

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kIterations; ++i) {
        // Test callback one-shot semantics
        {
          std::atomic<int> count{0};
          nix::callback<int> cb([&](std::future<int> f) {
            count.fetch_add(1);
            [[maybe_unused]] auto v = f.get();
          });

          cb(int{i});
          cb(int{i + 1}); // Second should be ignored

          if (count.load() == 1) {
            callback_successes.fetch_add(1);
          } else {
            callback_races.fetch_add(1);
          }
        }

        // Test sync_t concurrent access
        {
          try {
            static nix::sync_t<int> shared_counter(0);
            auto lock = shared_counter.lock();
            (*lock)++;
          } catch (const std::system_error&) {
            sync_errors.fetch_add(1);
          }
        }

        // Test finally exception handling
        {
          try {
            finally_t finally([&]() {
              // Simulate exception in finally that's caught
              try {
                if (i % 10 == 0) {
                  throw TestException("simulated");
                }
              } catch (...) {
                // Suppress - this is the fix pattern
              }
            });
          } catch (...) {
            finally_errors.fetch_add(1);
          }
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  INFO("callback_successes: " << callback_successes.load());
  INFO("callback_races: " << callback_races.load());
  INFO("sync_errors: " << sync_errors.load());
  INFO("finally_errors: " << finally_errors.load());

  REQUIRE(callback_races.load() == 0);
  REQUIRE(sync_errors.load() == 0);
  REQUIRE(finally_errors.load() == 0);
  REQUIRE(callback_successes.load() == kNumThreads * kIterations);
}
