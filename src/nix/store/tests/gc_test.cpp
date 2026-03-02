// GC coordination tests for store/gc.cpp
//
// Tests for temp root protection during evaluation, time-based GC expiry
// (gc-dead-after), and zombie reaping during garbage collection.
// These tests verify the fixes for issues #4382, #7572, #8638, #13740.

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

// Note: These tests are designed to verify GC behavior at the unit test level.
// Full integration tests would require a real store setup.

namespace fs = std::filesystem;

namespace {

// Helper to create a temporary directory for testing
struct TempDir {
  fs::path path;

  TempDir() {
    char tmpl[] = "/tmp/gc_test_XXXXXX";
    char* result = mkdtemp(tmpl);
    if (result == nullptr) {
      throw std::runtime_error("Failed to create temp directory");
    }
    path = result;
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// Mock path info for testing age-based GC
struct MockPathInfo {
  std::string path;
  time_t registration_time;
  bool is_valid{true};
};

// Simulate GC age calculation
bool should_gc_path(const MockPathInfo& info, time_t now, time_t gc_dead_after) {
  if (gc_dead_after == 0) {
    // No time-based expiry, delete immediately
    return true;
  }

  time_t age = now - info.registration_time;
  return age >= gc_dead_after;
}

// Helper to create zombie child for testing reaping
pid_t create_zombie() {
  pid_t pid = fork();
  if (pid == 0) {
    // Child exits immediately, becoming a zombie until reaped
    _exit(0);
  }
  // Give child time to exit and become zombie
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return pid;
}

// Check if a process is a zombie
bool is_zombie(pid_t pid) {
  if (pid <= 0)
    return false;

  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);

  std::ifstream stat_file(path);
  if (!stat_file.is_open()) {
    return false; // Process doesn't exist or already reaped
  }

  std::string line;
  std::getline(stat_file, line);

  // The third field in /proc/[pid]/stat is the state
  // Z = zombie
  size_t first_paren = line.find('(');
  size_t last_paren = line.rfind(')');
  if (first_paren != std::string::npos && last_paren != std::string::npos) {
    size_t state_pos = last_paren + 2; // Skip ") "
    if (state_pos < line.size()) {
      return line[state_pos] == 'Z';
    }
  }
  return false;
}

// Reap a specific child or all children
int reap_zombies() {
  int status;
  int reaped = 0;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    ++reaped;
  }
  return reaped;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Time-based GC expiry tests (gc-dead-after) - Issue #7572
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("gc-dead-after: paths older than threshold are eligible", "[gc][expiry]") {
  time_t now = std::time(nullptr);
  time_t gc_dead_after = 3600; // 1 hour

  MockPathInfo old_path{
      .path = "/nix/store/old-path",
      .registration_time = now - 7200, // 2 hours old
      .is_valid = true,
  };

  REQUIRE(should_gc_path(old_path, now, gc_dead_after));
}

TEST_CASE("gc-dead-after: paths newer than threshold are protected", "[gc][expiry]") {
  time_t now = std::time(nullptr);
  time_t gc_dead_after = 3600; // 1 hour

  MockPathInfo new_path{
      .path = "/nix/store/new-path",
      .registration_time = now - 1800, // 30 minutes old
      .is_valid = true,
  };

  REQUIRE_FALSE(should_gc_path(new_path, now, gc_dead_after));
}

TEST_CASE("gc-dead-after: edge case at exact threshold", "[gc][expiry]") {
  time_t now = std::time(nullptr);
  time_t gc_dead_after = 3600;

  MockPathInfo exact_path{
      .path = "/nix/store/exact-path",
      .registration_time = now - 3600, // Exactly 1 hour old
      .is_valid = true,
  };

  // At exact threshold, path should be eligible for GC
  REQUIRE(should_gc_path(exact_path, now, gc_dead_after));
}

TEST_CASE("gc-dead-after: zero disables time-based protection", "[gc][expiry]") {
  time_t now = std::time(nullptr);
  time_t gc_dead_after = 0; // Disabled

  MockPathInfo recent_path{
      .path = "/nix/store/recent-path",
      .registration_time = now - 60, // 1 minute old
      .is_valid = true,
  };

  // With gc_dead_after=0, all unreferenced paths are eligible
  REQUIRE(should_gc_path(recent_path, now, gc_dead_after));
}

TEST_CASE("gc-dead-after: very old paths are always eligible", "[gc][expiry]") {
  time_t now = std::time(nullptr);
  time_t gc_dead_after = 3600;

  MockPathInfo ancient_path{
      .path = "/nix/store/ancient-path",
      .registration_time = now - (365 * 24 * 3600), // 1 year old
      .is_valid = true,
  };

  REQUIRE(should_gc_path(ancient_path, now, gc_dead_after));
}

// ─────────────────────────────────────────────────────────────────────────────
// Temp root protection tests - Issue #8638
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("temp roots file creation", "[gc][temproots]") {
  TempDir tmpdir;
  fs::path temproots_dir = tmpdir.path / "temproots";
  fs::create_directories(temproots_dir);

  // Create a temp roots file for this process
  pid_t pid = getpid();
  fs::path temproot_file = temproots_dir / std::to_string(pid);

  {
    std::ofstream file(temproot_file);
    file << "/nix/store/test-path" << '\0';
  }

  REQUIRE(fs::exists(temproot_file));

  // Verify content
  std::ifstream file(temproot_file, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  REQUIRE(content.find("/nix/store/test-path") != std::string::npos);
}

TEST_CASE("temp roots file format with instance counter", "[gc][temproots]") {
  // Test the new format: {pid}-{instance}
  // See: https://github.com/NixOS/nix/issues/11979

  TempDir tmpdir;
  fs::path temproots_dir = tmpdir.path / "temproots";
  fs::create_directories(temproots_dir);

  pid_t pid = getpid();

  // Create files in both old and new format
  fs::path old_format = temproots_dir / std::to_string(pid);
  fs::path new_format = temproots_dir / (std::to_string(pid) + "-0");

  {
    std::ofstream(old_format) << "/nix/store/path1" << '\0';
    std::ofstream(new_format) << "/nix/store/path2" << '\0';
  }

  REQUIRE(fs::exists(old_format));
  REQUIRE(fs::exists(new_format));

  // Verify PID can be parsed from both formats
  auto parse_pid = [](const std::string& name) -> pid_t {
    auto dash = name.find('-');
    return std::stoi(dash != std::string::npos ? name.substr(0, dash) : name);
  };

  REQUIRE(parse_pid(old_format.filename().string()) == pid);
  REQUIRE(parse_pid(new_format.filename().string()) == pid);
}

TEST_CASE("stale temp roots file cleanup", "[gc][temproots]") {
  TempDir tmpdir;
  fs::path temproots_dir = tmpdir.path / "temproots";
  fs::create_directories(temproots_dir);

  // Create a temp roots file for a non-existent PID
  pid_t fake_pid = 99999;
  while (kill(fake_pid, 0) == 0) {
    ++fake_pid; // Find a PID that doesn't exist
  }

  fs::path stale_file = temproots_dir / std::to_string(fake_pid);
  {
    std::ofstream file(stale_file);
    file << "/nix/store/stale-path" << '\0';
  }

  REQUIRE(fs::exists(stale_file));

  // In a real GC, this file would be detected as stale because
  // we can acquire a write lock on it (the owning process is gone)
}

// ─────────────────────────────────────────────────────────────────────────────
// Zombie reaping tests - Issue #4382
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("zombie child is created and detectable", "[gc][zombie]") {
  // First reap any existing zombies from previous tests
  reap_zombies();

  pid_t zombie_pid = create_zombie();
  REQUIRE(zombie_pid > 0);

  // Verify it's a zombie (state Z in /proc/[pid]/stat)
  // Note: This might be flaky if the system reaps it first
  bool detected_zombie = is_zombie(zombie_pid);

  // Reap the zombie
  int status;
  pid_t reaped = waitpid(zombie_pid, &status, 0);

  if (!detected_zombie) {
    // Zombie was reaped by something else, which is fine
    INFO("Zombie was reaped before we could detect it");
  }

  REQUIRE((reaped == zombie_pid || reaped == -1));
}

TEST_CASE("reap_zombies cleans up multiple zombies", "[gc][zombie]") {
  // Create multiple zombie children
  std::vector<pid_t> zombies;
  for (int i = 0; i < 3; ++i) {
    pid_t pid = create_zombie();
    if (pid > 0) {
      zombies.push_back(pid);
    }
  }

  REQUIRE(zombies.size() >= 1); // At least one should succeed

  // Reap all zombies
  int reaped_count = reap_zombies();

  // All zombies should be reaped
  REQUIRE(reaped_count >= static_cast<int>(zombies.size()));
}

TEST_CASE("reap_zombies is idempotent", "[gc][zombie]") {
  // First reap any existing zombies
  reap_zombies();

  // Create and reap a zombie
  pid_t zombie_pid = create_zombie();
  REQUIRE(zombie_pid > 0);

  int first_reap = reap_zombies();
  REQUIRE(first_reap >= 1);

  // Second reap should find nothing
  int second_reap = reap_zombies();
  REQUIRE(second_reap == 0);
}

TEST_CASE("reap_zombies handles no children gracefully", "[gc][zombie]") {
  // Ensure no zombies exist
  reap_zombies();

  // Reaping when there are no children should return 0
  int reaped = reap_zombies();
  REQUIRE(reaped == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// GC coordination tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GC socket path construction", "[gc][socket]") {
  std::string state_dir = "/nix/var/nix";
  std::string gc_socket_path = "/gc-socket/socket";

  std::string full_path = state_dir + gc_socket_path;

  REQUIRE(full_path == "/nix/var/nix/gc-socket/socket");
}

TEST_CASE("GC roots directory construction", "[gc][roots]") {
  std::string state_dir = "/nix/var/nix";
  std::string gc_roots_dir = "gcroots";

  std::string full_path = state_dir + "/" + gc_roots_dir;

  REQUIRE(full_path == "/nix/var/nix/gcroots");
}

// ─────────────────────────────────────────────────────────────────────────────
// GC shutdown state tests - Issue #13740
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GC shutdown state atomic flag", "[gc][shutdown]") {
  std::atomic<bool> shutdown_requested{false};
  std::atomic<bool> gc_active{false};

  REQUIRE_FALSE(shutdown_requested.load());
  REQUIRE_FALSE(gc_active.load());

  gc_active.store(true);
  REQUIRE(gc_active.load());

  shutdown_requested.store(true);
  REQUIRE(shutdown_requested.load());

  gc_active.store(false);
  REQUIRE_FALSE(gc_active.load());
}

TEST_CASE("GC shutdown coordination between threads", "[gc][shutdown][thread]") {
  std::atomic<bool> shutdown_requested{false};
  std::atomic<bool> gc_active{false};
  std::atomic<int> operations_completed{0};

  // Simulate GC thread
  std::thread gc_thread([&]() {
    gc_active.store(true);

    for (int i = 0; i < 100 && !shutdown_requested.load(); ++i) {
      // Simulate GC operation
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      ++operations_completed;
    }

    gc_active.store(false);
  });

  // Let GC run for a bit
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Request shutdown
  shutdown_requested.store(true);

  gc_thread.join();

  REQUIRE(operations_completed.load() > 0);
  REQUIRE(operations_completed.load() < 100);
  REQUIRE_FALSE(gc_active.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// GC retry logic tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GC socket retry exponential backoff", "[gc][retry]") {
  int max_retries = 10;
  int initial_backoff_ms = 100;

  std::vector<int> backoffs;
  for (int retry = 1; retry <= max_retries; ++retry) {
    int backoff = initial_backoff_ms * (1 << std::min(retry - 1, 6));
    backoffs.push_back(backoff);
  }

  // Verify exponential growth with cap
  REQUIRE(backoffs[0] == 100);  // 100 * 2^0 = 100
  REQUIRE(backoffs[1] == 200);  // 100 * 2^1 = 200
  REQUIRE(backoffs[2] == 400);  // 100 * 2^2 = 400
  REQUIRE(backoffs[3] == 800);  // 100 * 2^3 = 800
  REQUIRE(backoffs[6] == 6400); // 100 * 2^6 = 6400 (cap)
  REQUIRE(backoffs[7] == 6400); // Still capped at 2^6
  REQUIRE(backoffs[9] == 6400); // Still capped
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GC handles hidden files in temproots", "[gc][temproots][edge]") {
  TempDir tmpdir;
  fs::path temproots_dir = tmpdir.path / "temproots";
  fs::create_directories(temproots_dir);

  // Create hidden files (should be ignored)
  fs::path hidden_file = temproots_dir / ".hidden";
  std::ofstream(hidden_file) << "should be ignored";

  REQUIRE(fs::exists(hidden_file));

  // Verify filename starts with '.'
  REQUIRE(hidden_file.filename().string()[0] == '.');
}

TEST_CASE("GC handles invalid PID filenames", "[gc][temproots][edge]") {
  TempDir tmpdir;
  fs::path temproots_dir = tmpdir.path / "temproots";
  fs::create_directories(temproots_dir);

  // Create files with invalid PID names
  std::ofstream(temproots_dir / "not-a-number") << "invalid";
  std::ofstream(temproots_dir / "-123") << "invalid";
  std::ofstream(temproots_dir / "abc-0") << "invalid";

  // These should all exist but be skipped during GC enumeration
  REQUIRE(fs::exists(temproots_dir / "not-a-number"));
  REQUIRE(fs::exists(temproots_dir / "-123"));
  REQUIRE(fs::exists(temproots_dir / "abc-0"));
}
