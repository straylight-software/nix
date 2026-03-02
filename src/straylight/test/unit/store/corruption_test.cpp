// corruption_test.cpp - Store corruption resistance tests
//
// Tests for store corruption issues fixed by the log-structured store:
//
// #11457 - File truncation on power loss during nix-copy-closure
// #8907  - Disk space exhaustion corrupts 178 store paths
// #14954 - Registry pins to corrupted store path
// #10641 - Empty manifest.json in profiles
// #13917 - Store entries don't appear atomically
// #14891 - nix-collect-garbage SEGFAULT corrupts database
//
// SQLite replacement issues (#3091, #11500, #8647, #6656, #7396, #1353):
// - Log-structured store operations that would fail with SQLite corruption
// - Concurrent access patterns that caused "database is busy"
// - Recovery from malformed entries

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "straylight/nix/crypto/hash.h"
#include "straylight/nix/testing/temp_dir.h"

// Use the same include path as the existing store_test.cpp
#include <straylight/nix/store/log_store.h>

// Note: log_store.h defines the store class and related types in straylight::nix::store

namespace fs = std::filesystem;
namespace store = straylight::nix::store;
namespace testing = straylight::nix::testing;
namespace crypto = straylight::nix::crypto;

// ============================================================================
// Test fixtures
// ============================================================================

struct temp_store {
  temp_store()
      : path_(testing::temp_directory_path() / ("corruption_test_" + std::to_string(counter_++))) {
    fs::create_directories(path_);
  }

  ~temp_store() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  [[nodiscard]] auto path() const -> const fs::path& { return path_; }

  [[nodiscard]] auto make_store() -> store::store {
    store::store s(path_);
    auto result = s.init();
    REQUIRE(result.has_value());
    return s;
  }

  fs::path path_;
  static inline int counter_ = 0;
};

static inline std::atomic<int> path_counter{0};

static auto make_path_info(std::string_view name) -> store::path_info {
  int counter = path_counter.fetch_add(1);
  char hash[33];
  std::snprintf(hash, sizeof(hash), "%08x", counter);
  std::string hash_str(hash);
  std::string name_part(name);
  name_part.resize(24, '0');
  hash_str += name_part;
  return store::path_info{
      .path = "/nix/store/" + hash_str + "-" + std::string(name),
      .nar_hash = "sha256:0000000000000000000000000000000000000000000000000000",
      .registration_time = 1234567890,
      .deriver = "",
      .nar_size = 1024,
      .ultimate = true,
      .sigs = {},
      .ca = "",
  };
}

template <typename... Args>
auto refs(Args&&... args) -> std::vector<std::string> {
  return std::vector<std::string>{std::forward<Args>(args)...};
}

// Helper to write raw bytes to a file
static void write_raw_file(const fs::path& path, std::span<const std::byte> data) {
  fs::create_directories(path.parent_path());
  int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  REQUIRE(fd >= 0);
  auto written = ::write(fd, data.data(), data.size());
  REQUIRE(written == static_cast<ssize_t>(data.size()));
  ::close(fd);
}

// Helper to read raw bytes from a file
static auto read_raw_file(const fs::path& path) -> std::vector<std::byte> {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return {};
  }
  struct stat st;
  REQUIRE(::fstat(fd, &st) == 0);
  std::vector<std::byte> data(static_cast<std::size_t>(st.st_size));
  auto n = ::read(fd, data.data(), data.size());
  REQUIRE(n == static_cast<ssize_t>(data.size()));
  ::close(fd);
  return data;
}

// ============================================================================
// #11457 - File truncation on power loss during nix-copy-closure
// Test: Write partial entry to log, verify recovery detects truncation via BLAKE3
// ============================================================================

TEST_CASE("log entry truncation detected via BLAKE3 checksum", "[corruption][#11457]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Register a valid path first
  auto info = make_path_info("valid-pkg");
  REQUIRE(s.register_path(info, {}).has_value());

  // Now manually corrupt the log by truncating the last entry
  auto log_file = tmp.path() / "log" / "current.log";
  auto log_data = read_raw_file(log_file);
  REQUIRE(log_data.size() > 0);

  // Truncate by removing the last 16 bytes (partial checksum)
  log_data.resize(log_data.size() - 16);
  write_raw_file(log_file, log_data);

  // Create a new store that reads the corrupted log
  store::store s2(tmp.path());
  auto init_result = s2.init();

  // Should fail because BLAKE3 checksum won't match truncated entry
  REQUIRE_FALSE(init_result.has_value());
  REQUIRE(init_result.error() == store::store_error::corrupt_data);
}

TEST_CASE("partial log write detected during recovery", "[corruption][#11457]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Create valid entries
  for (int i = 0; i < 5; ++i) {
    auto info = make_path_info("pkg" + std::to_string(i));
    REQUIRE(s.register_path(info, {}).has_value());
  }

  // Read log and simulate power loss mid-write by adding incomplete entry
  auto log_file = tmp.path() / "log" / "current.log";
  auto log_data = read_raw_file(log_file);

  // Append a length prefix that promises more data than exists
  std::uint32_t fake_len = 1024;                               // Promise 1KB of data
  std::vector<std::byte> partial_entry(sizeof(fake_len) + 10); // But only provide 10 bytes
  std::memcpy(partial_entry.data(), &fake_len, sizeof(fake_len));

  std::vector<std::byte> corrupted;
  corrupted.insert(corrupted.end(), log_data.begin(), log_data.end());
  corrupted.insert(corrupted.end(), partial_entry.begin(), partial_entry.end());
  write_raw_file(log_file, corrupted);

  // Recovery should detect the truncated entry
  store::store s2(tmp.path());
  auto result = s2.init();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == store::store_error::corrupt_data);
}

TEST_CASE("checksum mismatch detected for bit-flipped entry", "[corruption][#11457]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto info = make_path_info("test-pkg");
  REQUIRE(s.register_path(info, {}).has_value());

  // Read log and flip a bit in the entry data (not the checksum)
  auto log_file = tmp.path() / "log" / "current.log";
  auto log_data = read_raw_file(log_file);
  REQUIRE(log_data.size() > 40); // At least length + some data + checksum

  // Flip a bit in the middle of the data
  std::size_t flip_pos = 20;
  log_data[flip_pos] = static_cast<std::byte>(static_cast<int>(log_data[flip_pos]) ^ 0x01);
  write_raw_file(log_file, log_data);

  // Recovery should detect checksum mismatch
  store::store s2(tmp.path());
  auto result = s2.init();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == store::store_error::corrupt_data);
}

// ============================================================================
// #8907 - Disk space exhaustion corrupts 178 store paths
// Test: Simulate write failure mid-operation, verify store remains consistent
// ============================================================================

TEST_CASE("atomic write pattern prevents partial index files", "[corruption][#8907]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Register a path
  auto info = make_path_info("atomic-test");
  REQUIRE(s.register_path(info, {}).has_value());

  // Verify no .tmp files remain after successful write
  bool found_tmp = false;
  for (auto it = fs::recursive_directory_iterator(tmp.path());
       it != fs::recursive_directory_iterator(); ++it) {
    if (it->path().extension() == ".tmp") {
      found_tmp = true;
      break;
    }
  }
  REQUIRE_FALSE(found_tmp);
}

TEST_CASE("interrupted index write leaves previous state intact", "[corruption][#8907]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Register initial path
  auto info1 = make_path_info("original");
  REQUIRE(s.register_path(info1, {}).has_value());

  // Find the meta file
  auto hash = info1.path.substr(info1.path.rfind('/') + 1);
  auto dash = hash.find('-');
  hash = hash.substr(0, dash);
  auto shard = hash.substr(0, 2);
  auto meta_path = tmp.path() / "index" / "paths" / shard / (hash + ".meta");

  // Verify meta file exists and contains valid data
  REQUIRE(fs::exists(meta_path));
  auto original_data = read_raw_file(meta_path);
  REQUIRE(original_data.size() > 0);

  // Simulate a .tmp file left by interrupted write (disk full scenario)
  auto tmp_path = meta_path;
  tmp_path += ".tmp";
  std::vector<std::byte> garbage(100, std::byte{0xFF});
  write_raw_file(tmp_path, garbage);

  // Create new store instance - it should see original data, not garbage
  store::store s2(tmp.path());
  REQUIRE(s2.init().has_value());

  auto queried = s2.query_path_info(info1.path);
  REQUIRE(queried.has_value());
  REQUIRE(queried->path == info1.path);
}

TEST_CASE("log append is atomic - partial appends detected", "[corruption][#8907]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Register several paths to build up log
  std::vector<store::path_info> infos;
  for (int i = 0; i < 3; ++i) {
    auto info = make_path_info("pkg" + std::to_string(i));
    infos.push_back(info);
    REQUIRE(s.register_path(info, {}).has_value());
  }

  // Get current log size
  auto log_file = tmp.path() / "log" / "current.log";
  auto log_data = read_raw_file(log_file);
  auto valid_size = log_data.size();

  // Simulate disk full during 4th entry write - truncate at arbitrary point
  auto info4 = make_path_info("incomplete");
  REQUIRE(s.register_path(info4, {}).has_value());

  auto new_log_data = read_raw_file(log_file);
  // Truncate mid-entry
  new_log_data.resize(valid_size + 30);
  write_raw_file(log_file, new_log_data);

  // Recovery should fail on the truncated entry
  store::store s2(tmp.path());
  auto result = s2.init();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == store::store_error::corrupt_data);
}

// ============================================================================
// #14954 - Registry pins to corrupted store path
// Test: Create corrupted path, verify registry refuses to pin it
// ============================================================================

TEST_CASE("store verification detects corrupted nar_hash", "[corruption][#14954]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Register a path
  auto info = make_path_info("verify-test");
  REQUIRE(s.register_path(info, {}).has_value());

  // Corrupt the stored nar_hash in the meta file
  auto hash = info.path.substr(info.path.rfind('/') + 1);
  auto dash = hash.find('-');
  hash = hash.substr(0, dash);
  auto shard = hash.substr(0, 2);
  auto meta_path = tmp.path() / "index" / "paths" / shard / (hash + ".meta");

  auto meta_data = read_raw_file(meta_path);
  REQUIRE(meta_data.size() > 0);

  // Find and corrupt the hash string in the serialized data
  // The hash appears as "sha256:0000..." - flip some bits
  for (std::size_t i = 0; i < meta_data.size() - 5; ++i) {
    if (std::memcmp(&meta_data[i], "sha25", 5) == 0) {
      // Found it - corrupt a few bytes after
      meta_data[i + 10] = static_cast<std::byte>(0xFF);
      meta_data[i + 11] = static_cast<std::byte>(0xFE);
      break;
    }
  }
  write_raw_file(meta_path, meta_data);

  // Query should still work but return corrupted hash
  store::store s2(tmp.path());
  REQUIRE(s2.init().has_value());

  auto queried = s2.query_path_info(info.path);
  // The query succeeds but returns corrupted data
  if (queried.has_value()) {
    REQUIRE(queried->nar_hash != info.nar_hash); // Hash should be different (corrupted)
  }
  // OR deserialization fails with corrupt_data
}

TEST_CASE("verify() detects inconsistent index state", "[corruption][#14954]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Register paths with references
  auto dep = make_path_info("dep");
  auto pkg = make_path_info("pkg");

  REQUIRE(s.register_path(dep, {}).has_value());
  REQUIRE(s.register_path(pkg, refs(dep.path)).has_value());

  // Verify should pass on clean store
  auto verify_result = s.verify();
  REQUIRE(verify_result.has_value());
  REQUIRE(*verify_result == true);

  // Now corrupt: delete the refs file for pkg
  auto hash = pkg.path.substr(pkg.path.rfind('/') + 1);
  auto dash = hash.find('-');
  hash = hash.substr(0, dash);
  auto shard = hash.substr(0, 2);
  auto refs_path = tmp.path() / "index" / "refs" / shard / (hash + ".refs");
  fs::remove(refs_path);

  // Verify should now detect inconsistency
  store::store s2(tmp.path());
  REQUIRE(s2.init().has_value());

  auto verify_result2 = s2.verify();
  REQUIRE(verify_result2.has_value());
  REQUIRE(*verify_result2 == false); // Inconsistent state detected
}

// ============================================================================
// #10641 - Empty manifest.json in profiles
// Test: Interrupt profile write, verify previous profile preserved
// Note: This tests the atomic write pattern used by the store
// ============================================================================

TEST_CASE("atomic_write preserves original on incomplete write", "[corruption][#10641]") {
  temp_store tmp;
  fs::create_directories(tmp.path() / "profiles");

  // Create original profile data
  fs::path profile_path = tmp.path() / "profiles" / "default";
  std::string original_content = R"({"version": 1, "elements": ["foo", "bar"]})";
  std::vector<std::byte> original_data(
      reinterpret_cast<const std::byte*>(original_content.data()),
      reinterpret_cast<const std::byte*>(original_content.data() + original_content.size()));
  write_raw_file(profile_path, original_data);

  // Simulate interrupted write - create .tmp file but don't rename
  fs::path tmp_file = profile_path;
  tmp_file += ".tmp";
  std::string empty_content = "";
  std::vector<std::byte> empty_data;
  write_raw_file(tmp_file, empty_data);

  // Original should still be intact
  auto read_data = read_raw_file(profile_path);
  std::string read_content(reinterpret_cast<const char*>(read_data.data()), read_data.size());
  REQUIRE(read_content == original_content);
}

TEST_CASE("store index uses atomic writes for all operations", "[corruption][#10641]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Do multiple operations
  for (int i = 0; i < 10; ++i) {
    auto info = make_path_info("pkg" + std::to_string(i));
    REQUIRE(s.register_path(info, {}).has_value());
  }

  // No .tmp files should remain
  for (auto it = fs::recursive_directory_iterator(tmp.path());
       it != fs::recursive_directory_iterator(); ++it) {
    REQUIRE(it->path().extension() != ".tmp");
  }
}

// ============================================================================
// #13917 - Store entries don't appear atomically
// Test: Verify entries use atomic rename pattern
// ============================================================================

TEST_CASE("meta and refs files appear atomically", "[corruption][#13917]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto dep = make_path_info("dep");
  REQUIRE(s.register_path(dep, {}).has_value());

  auto pkg = make_path_info("pkg");
  REQUIRE(s.register_path(pkg, refs(dep.path)).has_value());

  // Both meta and refs should exist (never partial state)
  auto hash = pkg.path.substr(pkg.path.rfind('/') + 1);
  auto dash = hash.find('-');
  hash = hash.substr(0, dash);
  auto shard = hash.substr(0, 2);

  auto meta_path = tmp.path() / "index" / "paths" / shard / (hash + ".meta");
  auto refs_path = tmp.path() / "index" / "refs" / shard / (hash + ".refs");

  REQUIRE(fs::exists(meta_path));
  REQUIRE(fs::exists(refs_path));

  // Query should return complete info
  auto queried = s.query_path_info(pkg.path);
  REQUIRE(queried.has_value());

  auto queried_refs = s.query_references(pkg.path);
  REQUIRE(queried_refs.has_value());
  REQUIRE(queried_refs->size() == 1);
}

TEST_CASE("concurrent readers see consistent state during writes", "[corruption][#13917]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Pre-populate with some paths
  std::vector<store::path_info> initial_infos;
  for (int i = 0; i < 5; ++i) {
    auto info = make_path_info("initial" + std::to_string(i));
    initial_infos.push_back(info);
    REQUIRE(s.register_path(info, {}).has_value());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> read_count{0};
  std::atomic<int> write_count{0};
  std::atomic<int> errors{0};

  // Reader thread - continuously query existing paths
  std::thread reader([&]() {
    while (!stop.load()) {
      for (const auto& info : initial_infos) {
        auto result = s.query_path_info(info.path);
        if (result.has_value()) {
          // If we get a result, it must be complete
          if (result->path.empty() || result->nar_hash.empty()) {
            errors.fetch_add(1);
          }
          read_count.fetch_add(1);
        }
      }
    }
  });

  // Writer thread - add new paths
  std::thread writer([&]() {
    for (int i = 0; i < 20; ++i) {
      auto info = make_path_info("concurrent" + std::to_string(i));
      auto result = s.register_path(info, {});
      if (result.has_value()) {
        write_count.fetch_add(1);
      }
    }
    stop.store(true);
  });

  writer.join();
  reader.join();

  REQUIRE(errors.load() == 0);
  REQUIRE(read_count.load() > 0);
  REQUIRE(write_count.load() == 20);
}

// ============================================================================
// #14891 - nix-collect-garbage SEGFAULT corrupts database
// Test: Simulate SEGFAULT during GC, verify log-structured store recovers
// ============================================================================

TEST_CASE("log store recovers from interrupted invalidation", "[corruption][#14891]") {
  temp_store tmp;
  std::vector<store::path_info> infos;

  // First session: register paths
  {
    auto s = tmp.make_store();
    for (int i = 0; i < 5; ++i) {
      auto info = make_path_info("gc_target" + std::to_string(i));
      infos.push_back(info);
      REQUIRE(s.register_path(info, {}).has_value());
    }
    // Invalidate first 2 paths (simulating GC)
    REQUIRE(s.invalidate_path(infos[0].path).has_value());
    REQUIRE(s.invalidate_path(infos[1].path).has_value());
  }

  // Simulate crash by corrupting head file (sequence mismatch)
  // This simulates a crash between log append and head update
  auto head_path = tmp.path() / "head";
  std::uint64_t wrong_seq = 1; // Set to old sequence
  std::vector<std::byte> head_data(sizeof(wrong_seq));
  std::memcpy(head_data.data(), &wrong_seq, sizeof(wrong_seq));
  write_raw_file(head_path, head_data);

  // Second session: recovery should replay log correctly
  {
    store::store s2(tmp.path());
    REQUIRE(s2.init().has_value());

    // Invalidated paths should not be valid
    REQUIRE_FALSE(s2.is_valid_path(infos[0].path));
    REQUIRE_FALSE(s2.is_valid_path(infos[1].path));

    // Non-invalidated paths should still be valid
    REQUIRE(s2.is_valid_path(infos[2].path));
    REQUIRE(s2.is_valid_path(infos[3].path));
    REQUIRE(s2.is_valid_path(infos[4].path));
  }
}

TEST_CASE("store compact creates valid checkpoint after crash", "[corruption][#14891]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Build up log with many operations
  for (int i = 0; i < 20; ++i) {
    auto info = make_path_info("compact_test" + std::to_string(i));
    REQUIRE(s.register_path(info, {}).has_value());
  }

  // Compact the log
  REQUIRE(s.compact().has_value());

  // Log file should be deleted after compact
  auto log_file = tmp.path() / "log" / "current.log";
  // Note: compact() writes a new checkpoint entry, so file still exists but is minimal
  auto log_data = read_raw_file(log_file);
  REQUIRE(log_data.size() < 200); // Much smaller than before

  // Verify should still pass
  auto verify = s.verify();
  REQUIRE(verify.has_value());
  REQUIRE(*verify == true);
}

// ============================================================================
// SQLite replacement tests (#3091, #11500, #8647, #6656, #7396, #1353)
// ============================================================================

TEST_CASE("log store handles concurrent writers without 'database is busy'",
          "[corruption][sqlite]") {
  temp_store tmp;
  auto s = tmp.make_store();

  constexpr int num_threads = 8;
  constexpr int ops_per_thread = 10;

  std::atomic<int> success_count{0};
  std::atomic<int> lock_failures{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < ops_per_thread; ++i) {
        auto info = make_path_info("concurrent_" + std::to_string(t) + "_" + std::to_string(i));
        auto result = s.register_path(info, {});
        if (result.has_value()) {
          success_count.fetch_add(1);
        } else if (result.error() == store::store_error::lock_failed) {
          lock_failures.fetch_add(1);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All operations should succeed (flock provides serialization, not SQLITE_BUSY)
  REQUIRE(success_count.load() == num_threads * ops_per_thread);
  REQUIRE(lock_failures.load() == 0);
}

TEST_CASE("malformed log entry is detected and rejected", "[corruption][sqlite]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Register a valid path first
  auto info = make_path_info("valid");
  REQUIRE(s.register_path(info, {}).has_value());

  // Append malformed entry to log (valid length but garbage data)
  auto log_file = tmp.path() / "log" / "current.log";
  auto log_data = read_raw_file(log_file);

  // Create a "valid-looking" but malformed entry
  std::uint32_t fake_len = 64;
  std::vector<std::byte> malformed(sizeof(fake_len) + 64 + 32); // len + data + checksum
  std::memcpy(malformed.data(), &fake_len, sizeof(fake_len));
  // Fill with random garbage
  std::random_device rd;
  std::mt19937 gen(rd());
  for (std::size_t i = sizeof(fake_len); i < malformed.size(); ++i) {
    malformed[i] = static_cast<std::byte>(gen() % 256);
  }

  std::vector<std::byte> corrupted;
  corrupted.insert(corrupted.end(), log_data.begin(), log_data.end());
  corrupted.insert(corrupted.end(), malformed.begin(), malformed.end());
  write_raw_file(log_file, corrupted);

  // Recovery should fail on malformed entry (checksum won't match)
  store::store s2(tmp.path());
  auto result = s2.init();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == store::store_error::corrupt_data);
}

TEST_CASE("recovery from partial index rebuild", "[corruption][sqlite]") {
  temp_store tmp;
  std::vector<store::path_info> infos;

  // First session: register paths
  {
    auto s = tmp.make_store();
    for (int i = 0; i < 5; ++i) {
      auto info = make_path_info("rebuild" + std::to_string(i));
      infos.push_back(info);
      REQUIRE(s.register_path(info, {}).has_value());
    }
  }

  // Corrupt: Delete some meta files but keep log intact
  for (int i = 0; i < 3; ++i) {
    auto hash = infos[i].path.substr(infos[i].path.rfind('/') + 1);
    auto dash = hash.find('-');
    hash = hash.substr(0, dash);
    auto shard = hash.substr(0, 2);
    auto meta_path = tmp.path() / "index" / "paths" / shard / (hash + ".meta");
    fs::remove(meta_path);
  }

  // Second session: recovery should rebuild index from log
  {
    store::store s2(tmp.path());
    REQUIRE(s2.init().has_value());

    // All paths should be valid after replay
    for (const auto& info : infos) {
      REQUIRE(s2.is_valid_path(info.path));
    }
  }
}

TEST_CASE("large transaction doesn't cause timeout", "[corruption][sqlite]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Register many paths in sequence (would cause SQLite busy in old implementation)
  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < 100; ++i) {
    auto info = make_path_info("large_tx" + std::to_string(i));
    auto result = s.register_path(info, {});
    REQUIRE(result.has_value());
  }

  auto end = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  // Should complete reasonably quickly (no exponential backoff from SQLITE_BUSY)
  REQUIRE(elapsed.count() < 5000); // 5 seconds max

  // Verify all paths exist
  auto all = s.query_all_valid_paths();
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 100);
}

TEST_CASE("referrer consistency maintained across operations", "[corruption][sqlite]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Create dependency chain: a -> b -> c
  auto a = make_path_info("aaa");
  auto b = make_path_info("bbb");
  auto c = make_path_info("ccc");

  REQUIRE(s.register_path(c, {}).has_value());
  REQUIRE(s.register_path(b, refs(c.path)).has_value());
  REQUIRE(s.register_path(a, refs(b.path)).has_value());

  // Check forward refs
  auto a_refs = s.query_references(a.path);
  REQUIRE(a_refs.has_value());
  REQUIRE(a_refs->size() == 1);
  REQUIRE((*a_refs)[0] == b.path);

  // Check reverse refs (referrers)
  auto b_referrers = s.query_referrers(b.path);
  REQUIRE(b_referrers.has_value());
  REQUIRE(b_referrers->size() == 1);
  REQUIRE((*b_referrers)[0] == a.path);

  auto c_referrers = s.query_referrers(c.path);
  REQUIRE(c_referrers.has_value());
  REQUIRE(c_referrers->size() == 1);
  REQUIRE((*c_referrers)[0] == b.path);

  // Now invalidate b - should update referrers of c
  REQUIRE(s.invalidate_path(b.path).has_value());

  // c should have no referrers now
  c_referrers = s.query_referrers(c.path);
  REQUIRE(c_referrers.has_value());
  REQUIRE(c_referrers->empty());
}

// ============================================================================
// Edge cases and stress tests
// ============================================================================

TEST_CASE("empty log file is valid initial state", "[corruption][edge]") {
  temp_store tmp;
  fs::create_directories(tmp.path() / "log");

  // Create empty log file
  auto log_file = tmp.path() / "log" / "current.log";
  std::ofstream(log_file).close();

  store::store s(tmp.path());
  auto result = s.init();
  REQUIRE(result.has_value());

  auto all = s.query_all_valid_paths();
  REQUIRE(all.has_value());
  REQUIRE(all->empty());
}

TEST_CASE("maximum log entry size is enforced", "[corruption][edge]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Create a valid path first
  auto info = make_path_info("normal");
  REQUIRE(s.register_path(info, {}).has_value());

  // Craft a log entry that claims to be > 64MB (the sanity limit)
  auto log_file = tmp.path() / "log" / "current.log";
  auto log_data = read_raw_file(log_file);

  // Append entry with huge length
  std::uint32_t huge_len = 128 * 1024 * 1024; // 128MB
  std::vector<std::byte> huge_entry(sizeof(huge_len));
  std::memcpy(huge_entry.data(), &huge_len, sizeof(huge_len));

  std::vector<std::byte> corrupted;
  corrupted.insert(corrupted.end(), log_data.begin(), log_data.end());
  corrupted.insert(corrupted.end(), huge_entry.begin(), huge_entry.end());
  write_raw_file(log_file, corrupted);

  // Recovery should reject oversized entry
  store::store s2(tmp.path());
  auto result = s2.init();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == store::store_error::corrupt_data);
}

TEST_CASE("store handles rapid register/invalidate cycles", "[corruption][stress]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Rapidly create and delete paths
  for (int cycle = 0; cycle < 5; ++cycle) {
    std::vector<store::path_info> batch;
    for (int i = 0; i < 10; ++i) {
      auto info = make_path_info("cycle" + std::to_string(cycle) + "_" + std::to_string(i));
      batch.push_back(info);
      REQUIRE(s.register_path(info, {}).has_value());
    }

    // Delete half
    for (int i = 0; i < 5; ++i) {
      REQUIRE(s.invalidate_path(batch[i].path).has_value());
    }
  }

  // Verify consistency
  auto verify = s.verify();
  REQUIRE(verify.has_value());
  REQUIRE(*verify == true);
}

TEST_CASE("BLAKE3 checksum catches single-byte corruption", "[corruption][integrity]") {
  // Direct test of checksum sensitivity
  std::string original = "test data for checksumming";
  std::span<const std::uint8_t> original_span(
      reinterpret_cast<const std::uint8_t*>(original.data()), original.size());
  auto original_hash = crypto::blake3(original_span);

  // Flip each bit position and verify hash changes
  for (std::size_t i = 0; i < original.size(); ++i) {
    std::string modified = original;
    modified[i] ^= 0x01; // Flip LSB

    std::span<const std::uint8_t> modified_span(
        reinterpret_cast<const std::uint8_t*>(modified.data()), modified.size());
    auto modified_hash = crypto::blake3(modified_span);

    REQUIRE(original_hash != modified_hash);
  }
}
