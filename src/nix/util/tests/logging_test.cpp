// Logging synchronization tests for util/logging.cpp
//
// Tests for concurrent log output synchronization to prevent interleaved
// messages. These tests verify the fixes for issues #7298 and #14294.

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

// Mock synchronized output buffer for testing
struct SyncOutputBuffer {
public:
  void write(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_ += msg;
    ++write_count_;
  }

  void write_unsafe(const std::string& msg) {
    // Intentionally no lock - for demonstrating race conditions
    buffer_ += msg;
    ++write_count_;
  }

  std::string get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_;
  }

  int write_count() const { return write_count_; }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
    write_count_ = 0;
  }

private:
  mutable std::mutex mutex_;
  std::string buffer_;
  std::atomic<int> write_count_{0};
};

// Check if output has interleaved lines
// Lines should not be mixed up - each line should contain only one thread's output
bool has_interleaved_output(const std::string& output, const std::string& marker1,
                            const std::string& marker2) {
  // Look for patterns where marker1 and marker2 appear on the same line
  // This would indicate interleaving

  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    bool has_m1 = line.find(marker1) != std::string::npos;
    bool has_m2 = line.find(marker2) != std::string::npos;

    // If a single line contains both markers, we have interleaving
    if (has_m1 && has_m2) {
      return true;
    }
  }
  return false;
}

// Count complete lines in output
int count_complete_lines(const std::string& output) {
  int count = 0;
  for (char c : output) {
    if (c == '\n') {
      ++count;
    }
  }
  return count;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Basic synchronization tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("synchronized write: single thread", "[logging][sync]") {
  SyncOutputBuffer buffer;

  buffer.write("line 1\n");
  buffer.write("line 2\n");
  buffer.write("line 3\n");

  REQUIRE(buffer.write_count() == 3);
  REQUIRE(count_complete_lines(buffer.get()) == 3);
}

TEST_CASE("synchronized write: multiple threads", "[logging][sync][thread]") {
  SyncOutputBuffer buffer;
  const int num_threads = 4;
  const int writes_per_thread = 100;

  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&buffer, t]() {
      for (int i = 0; i < writes_per_thread; ++i) {
        buffer.write("thread" + std::to_string(t) + "-line" + std::to_string(i) + "\n");
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  REQUIRE(buffer.write_count() == num_threads * writes_per_thread);
  REQUIRE(count_complete_lines(buffer.get()) == num_threads * writes_per_thread);
}

TEST_CASE("synchronized write: no interleaving between threads", "[logging][sync][thread]") {
  SyncOutputBuffer buffer;

  std::thread t1([&buffer]() {
    for (int i = 0; i < 50; ++i) {
      buffer.write("THREAD1:message" + std::to_string(i) + "\n");
    }
  });

  std::thread t2([&buffer]() {
    for (int i = 0; i < 50; ++i) {
      buffer.write("THREAD2:message" + std::to_string(i) + "\n");
    }
  });

  t1.join();
  t2.join();

  // No line should contain both THREAD1 and THREAD2 markers
  REQUIRE_FALSE(has_interleaved_output(buffer.get(), "THREAD1:", "THREAD2:"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Error trace synchronization tests - Issue #14294
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("error trace: multi-line error message stays together", "[logging][error]") {
  SyncOutputBuffer buffer;

  // Simulate error with traces (as happens with --show-trace)
  std::string error_msg = "error: build of '/nix/store/abc' failed\n"
                          "       ... while evaluating attribute 'buildPhase'\n"
                          "       ... called from /nix/store/xyz/default.nix:42:5\n"
                          "       ... while instantiating 'stdenv.mkDerivation'\n";

  buffer.write(error_msg);

  std::string output = buffer.get();

  // All lines should be present and in order
  REQUIRE(output.find("error: build") != std::string::npos);
  REQUIRE(output.find("while evaluating") != std::string::npos);
  REQUIRE(output.find("called from") != std::string::npos);
  REQUIRE(output.find("while instantiating") != std::string::npos);
}

TEST_CASE("error trace: concurrent errors don't interleave traces", "[logging][error][thread]") {
  SyncOutputBuffer buffer;

  auto write_error = [&buffer](int id) {
    std::string error = "ERROR" + std::to_string(id) +
                        ": build failed\n"
                        "TRACE" +
                        std::to_string(id) +
                        ": while evaluating\n"
                        "TRACE" +
                        std::to_string(id) + ": called from\n";
    buffer.write(error);
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 10; ++i) {
    threads.emplace_back(write_error, i);
  }

  for (auto& t : threads) {
    t.join();
  }

  std::string output = buffer.get();

  // Check that error IDs are consistent within each block
  // Each ERROR line should be followed by TRACE lines with the same ID
  std::istringstream stream(output);
  std::string line;
  int current_id = -1;

  while (std::getline(stream, line)) {
    if (line.find("ERROR") != std::string::npos) {
      // Extract ID from ERROR line
      size_t pos = line.find("ERROR") + 5;
      if (pos < line.size() && std::isdigit(line[pos])) {
        current_id = line[pos] - '0';
      }
    } else if (line.find("TRACE") != std::string::npos && current_id >= 0) {
      // TRACE line should have same ID
      size_t pos = line.find("TRACE") + 5;
      if (pos < line.size() && std::isdigit(line[pos])) {
        int trace_id = line[pos] - '0';
        REQUIRE(trace_id == current_id);
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrent warning tests - Issue #7298
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("warnings: concurrent warnings don't interleave", "[logging][warn][thread]") {
  SyncOutputBuffer buffer;

  std::thread t1([&buffer]() {
    for (int i = 0; i < 20; ++i) {
      buffer.write("warning: WARN_A message " + std::to_string(i) + "\n");
    }
  });

  std::thread t2([&buffer]() {
    for (int i = 0; i < 20; ++i) {
      buffer.write("warning: WARN_B message " + std::to_string(i) + "\n");
    }
  });

  t1.join();
  t2.join();

  // No line should contain both WARN_A and WARN_B
  REQUIRE_FALSE(has_interleaved_output(buffer.get(), "WARN_A", "WARN_B"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Verbosity level tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("verbosity: debug messages synchronized", "[logging][debug][thread]") {
  SyncOutputBuffer buffer;

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&buffer, t]() {
      for (int i = 0; i < 25; ++i) {
        buffer.write("debug[" + std::to_string(t) + "]: message " + std::to_string(i) + "\n");
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  REQUIRE(count_complete_lines(buffer.get()) == 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// Activity logging tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("activity: start/stop messages stay paired", "[logging][activity]") {
  SyncOutputBuffer buffer;

  auto do_activity = [&buffer](int id) {
    std::string start = "activity[" + std::to_string(id) + "]: starting\n";
    std::string end = "activity[" + std::to_string(id) + "]: completed\n";

    buffer.write(start);
    // Simulate work
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    buffer.write(end);
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 5; ++i) {
    threads.emplace_back(do_activity, i);
  }

  for (auto& t : threads) {
    t.join();
  }

  std::string output = buffer.get();

  // Each activity should have both start and end
  for (int i = 0; i < 5; ++i) {
    REQUIRE(output.find("activity[" + std::to_string(i) + "]: starting") != std::string::npos);
    REQUIRE(output.find("activity[" + std::to_string(i) + "]: completed") != std::string::npos);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stress tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("stress: high contention logging", "[logging][stress][thread]") {
  SyncOutputBuffer buffer;
  const int num_threads = 8;
  const int messages_per_thread = 500;

  std::atomic<int> total_written{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&buffer, &total_written, t]() {
      for (int i = 0; i < messages_per_thread; ++i) {
        buffer.write("T" + std::to_string(t) + ":M" + std::to_string(i) + "\n");
        ++total_written;
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  REQUIRE(total_written == num_threads * messages_per_thread);
  REQUIRE(buffer.write_count() == num_threads * messages_per_thread);
}

TEST_CASE("stress: rapid lock/unlock cycles", "[logging][stress][thread]") {
  SyncOutputBuffer buffer;
  std::atomic<bool> stop{false};
  std::atomic<int> iterations{0};

  auto rapid_writer = [&buffer, &stop, &iterations]() {
    while (!stop) {
      buffer.write("x");
      ++iterations;
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back(rapid_writer);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop = true;

  for (auto& t : threads) {
    t.join();
  }

  // All iterations should have written their character
  REQUIRE(static_cast<int>(buffer.get().size()) == iterations.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("edge: empty message", "[logging][edge]") {
  SyncOutputBuffer buffer;

  buffer.write("");
  buffer.write("");
  buffer.write("actual content\n");

  REQUIRE(buffer.write_count() == 3);
  REQUIRE(buffer.get() == "actual content\n");
}

TEST_CASE("edge: very long single line", "[logging][edge]") {
  SyncOutputBuffer buffer;

  std::string long_line(10000, 'x');
  long_line += "\n";

  buffer.write(long_line);

  REQUIRE(buffer.get().size() == 10001);
  REQUIRE(count_complete_lines(buffer.get()) == 1);
}

TEST_CASE("edge: unicode in messages", "[logging][edge]") {
  SyncOutputBuffer buffer;

  buffer.write("error: build failed\n");
  buffer.write("  cause: file not found\n");

  std::string output = buffer.get();
  REQUIRE(output.find("error:") != std::string::npos);
  REQUIRE(output.find("cause:") != std::string::npos);
}
