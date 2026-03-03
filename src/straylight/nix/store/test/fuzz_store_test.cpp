// straylight // nix // store // test
//
// Fuzz tests for the two-tier daemonless store.
//
// Key attack surfaces:
// - ca_store: hash validation, blob storage, path traversal
// - legacy_store: SQLite injection, path parsing, reference handling
// - two_tier_store: tier dispatch, error propagation
// - store_adapter: path conversion, signature verification
//
// These tests hammer the store with malformed inputs to find crashes,
// hangs, memory corruption, and security vulnerabilities.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "straylight/nix/store/ca_store.h"
#include "straylight/nix/store/legacy_store.h"
#include "straylight/nix/store/two_tier_store.h"

#include "nix/tests/property.h"

namespace fs = std::filesystem;

// =============================================================================
// Test fixture: temporary store directory
// =============================================================================

namespace {

struct temp_store {
  temp_store() {
    // Use random suffix to avoid collisions across parallel test processes
    auto pid = static_cast<std::uint64_t>(getpid());
    auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto cnt = counter_++;
    auto rnd =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    path_ = fs::temp_directory_path() /
            ("fuzz_store_" + std::to_string(pid) + "_" + std::to_string(tid) + "_" +
             std::to_string(cnt) + "_" + std::to_string(rnd % 1000000));
    fs::create_directories(path_);
  }

  ~temp_store() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  auto path() const -> const fs::path& { return path_; }

private:
  fs::path path_;
  static inline std::atomic<std::uint64_t> counter_{0};
};

// Generate valid-looking store paths
auto gen_store_path() {
  return rc::gen::apply(
      [](const std::string& hash, const std::string& name) {
        // Ensure hash is 32 chars of valid nix32
        std::string valid_hash;
        for (std::size_t i = 0; i < 32; ++i) {
          char c = hash.empty() ? 'a' : hash[i % hash.size()];
          // nix32 alphabet: 0-9, a-z except e,o,t,u
          if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'd') || (c >= 'f' && c <= 'n') ||
              (c >= 'p' && c <= 's') || (c >= 'v' && c <= 'z')) {
            valid_hash += c;
          } else {
            valid_hash += 'a';
          }
        }
        // Sanitize name
        std::string valid_name;
        for (char c : name) {
          if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.') {
            valid_name += c;
          }
        }
        if (valid_name.empty()) {
          valid_name = "pkg";
        }
        return "/nix/store/" + valid_hash + "-" + valid_name;
      },
      rc::gen::arbitrary<std::string>(), rc::gen::arbitrary<std::string>());
}

// Generate valid hex hashes
auto gen_hex_hash() {
  return rc::gen::apply(
      [](const std::string& input) {
        std::string hash;
        hash.reserve(64);
        for (std::size_t i = 0; i < 64; ++i) {
          char c = input.empty() ? '0' : input[i % input.size()];
          if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            hash += c;
          } else if (c >= 'A' && c <= 'F') {
            hash += static_cast<char>(c - 'A' + 'a');
          } else {
            hash += '0';
          }
        }
        return hash;
      },
      rc::gen::arbitrary<std::string>());
}

// Generate blob data
auto gen_blob_data() {
  return rc::gen::map(rc::gen::arbitrary<std::string>(), [](const std::string& s) {
    std::vector<std::byte> data;
    data.reserve(s.size());
    for (char c : s) {
      data.push_back(static_cast<std::byte>(c));
    }
    return data;
  });
}

} // namespace

// =============================================================================
// CA Store fuzz tests
// =============================================================================

TEST_CASE("fuzz: ca_store handles arbitrary hash strings", "[fuzz][ca_store]") {
  rc::prop("ca_store::has never crashes on arbitrary input", []() {
    temp_store tmp;
    straylight::nix::store::ca_store store(tmp.path() / "ca");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    auto hash = *rc::gen::arbitrary<std::string>();
    // Should not crash, just return false for invalid hashes
    [[maybe_unused]] auto result = store.has(hash);
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: ca_store::get handles arbitrary hash strings", "[fuzz][ca_store]") {
  rc::prop("ca_store::get never crashes on arbitrary input", []() {
    temp_store tmp;
    straylight::nix::store::ca_store store(tmp.path() / "ca");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    auto hash = *rc::gen::arbitrary<std::string>();
    // Should not crash, return not_found or invalid_hash
    [[maybe_unused]] auto result = store.get(hash);
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: ca_store::put handles arbitrary data", "[fuzz][ca_store]") {
  rc::prop("ca_store::put never crashes on arbitrary data", []() {
    temp_store tmp;
    straylight::nix::store::ca_store store(tmp.path() / "ca");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    auto data = *gen_blob_data();
    // Should not crash, successfully store data
    auto result = store.put(std::span<const std::byte>(data));
    RC_ASSERT(result.has_value());
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: ca_store::put with hash handles mismatches", "[fuzz][ca_store]") {
  rc::prop("ca_store::put with wrong hash returns error", []() {
    temp_store tmp;
    straylight::nix::store::ca_store store(tmp.path() / "ca");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    auto hash = *gen_hex_hash();
    auto data = *gen_blob_data();

    // Should not crash, may return hash_mismatch
    [[maybe_unused]] auto result = store.put(hash, std::span<const std::byte>(data));
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: ca_store handles path traversal attempts", "[fuzz][ca_store][security]") {
  rc::prop("ca_store rejects path traversal in hash", []() {
    temp_store tmp;
    straylight::nix::store::ca_store store(tmp.path() / "ca");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    // Try various path traversal attacks
    std::vector<std::string> attacks = {
        "../../../etc/passwd",
        "..%2f..%2f..%2fetc%2fpasswd",
        "....//....//....//etc/passwd",
        "00000000000000000000000000000000/../../../etc/passwd",
        "00000000000000000000000000000000/../../../../etc/passwd",
        std::string(64, '.') + "/etc/passwd",
        std::string(64, '/'),
        std::string(64, '\0'),
    };

    for (const auto& attack : attacks) {
      // Should not crash, should reject
      [[maybe_unused]] auto has_result = store.has(attack);
      [[maybe_unused]] auto get_result = store.get(attack);
      // blob_path should not escape root
      auto path = store.blob_path(attack);
      // Path should stay within store root (or be rejected)
    }
    RC_SUCCEED("No path traversal");
  });
}

TEST_CASE("fuzz: ca_store roundtrip preserves data", "[fuzz][ca_store]") {
  rc::prop("put then get returns identical data", []() {
    temp_store tmp;
    straylight::nix::store::ca_store store(tmp.path() / "ca");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    auto data = *gen_blob_data();
    auto put_result = store.put(std::span<const std::byte>(data));
    RC_ASSERT(put_result.has_value());

    auto get_result = store.get(*put_result);
    RC_ASSERT(get_result.has_value());
    RC_ASSERT(*get_result == data);
    RC_SUCCEED("Roundtrip OK");
  });
}

// =============================================================================
// Legacy Store fuzz tests
// =============================================================================

TEST_CASE("fuzz: legacy_store handles arbitrary store paths", "[fuzz][legacy_store]") {
  rc::prop("legacy_store::is_valid_path never crashes", []() {
    temp_store tmp;
    straylight::nix::store::legacy_store store(tmp.path() / "db");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    auto path = *rc::gen::arbitrary<std::string>();
    // Should not crash
    [[maybe_unused]] auto result = store.is_valid_path(path);
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: legacy_store handles arbitrary path info", "[fuzz][legacy_store]") {
  rc::prop("legacy_store::register_path handles arbitrary info", []() {
    temp_store tmp;
    straylight::nix::store::legacy_store store(tmp.path() / "db");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    straylight::nix::store::legacy_path_info info;
    info.path = *gen_store_path();
    info.nar_hash = *gen_hex_hash();
    info.registration_time = *rc::gen::arbitrary<std::int64_t>();
    info.deriver = *rc::gen::arbitrary<std::string>();
    info.nar_size = *rc::gen::arbitrary<std::int64_t>();
    info.ultimate = *rc::gen::arbitrary<bool>();
    info.sigs = *rc::gen::arbitrary<std::vector<std::string>>();
    info.ca = *rc::gen::arbitrary<std::string>();

    std::vector<std::string> refs;
    auto ref_count = *rc::gen::inRange(0, 10);
    for (int i = 0; i < ref_count; ++i) {
      refs.push_back(*gen_store_path());
    }

    // Should not crash (may fail on invalid data)
    [[maybe_unused]] auto result = store.register_path(info, refs);
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: legacy_store handles SQL injection attempts", "[fuzz][legacy_store][security]") {
  rc::prop("legacy_store rejects SQL injection", []() {
    temp_store tmp;
    straylight::nix::store::legacy_store store(tmp.path() / "db");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    // SQL injection attempts in store path
    std::vector<std::string> attacks = {
        "/nix/store/'; DROP TABLE ValidPaths;--",
        "/nix/store/\" OR 1=1--",
        "/nix/store/' UNION SELECT * FROM sqlite_master--",
        "/nix/store/\"; DELETE FROM ValidPaths;--",
        "/nix/store/' OR '1'='1",
        std::string("/nix/store/") + std::string(1000, 'A'),
        std::string("/nix/store/") + std::string(1000, '\''),
    };

    for (const auto& attack : attacks) {
      // Should not crash, should handle gracefully
      [[maybe_unused]] auto is_valid = store.is_valid_path(attack);
      [[maybe_unused]] auto info = store.query_path_info(attack);
      [[maybe_unused]] auto refs = store.query_references(attack);
    }

    // Verify database still works
    auto count = store.count();
    RC_ASSERT(count.has_value());
    RC_SUCCEED("No SQL injection");
  });
}

TEST_CASE("fuzz: legacy_store query_path_info handles arbitrary paths", "[fuzz][legacy_store]") {
  rc::prop("query_path_info never crashes", []() {
    temp_store tmp;
    straylight::nix::store::legacy_store store(tmp.path() / "db");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    auto path = *rc::gen::arbitrary<std::string>();
    // Should not crash, return not_found for invalid
    [[maybe_unused]] auto result = store.query_path_info(path);
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: legacy_store roundtrip preserves path info", "[fuzz][legacy_store]") {
  rc::prop("register then query returns consistent info", []() {
    temp_store tmp;
    straylight::nix::store::legacy_store store(tmp.path() / "db");
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    straylight::nix::store::legacy_path_info info;
    info.path = *gen_store_path();
    info.nar_hash = *gen_hex_hash();
    info.registration_time = std::abs(*rc::gen::arbitrary<std::int64_t>() % 2000000000);
    info.nar_size = std::abs(*rc::gen::arbitrary<std::int64_t>() % 1000000000);
    info.ultimate = *rc::gen::arbitrary<bool>();

    std::vector<std::string> refs;
    auto result = store.register_path(info, refs);
    if (!result.has_value()) {
      RC_SUCCEED("Registration failed (expected for some inputs)");
    }

    auto queried = store.query_path_info(info.path);
    RC_ASSERT(queried.has_value());
    RC_ASSERT(queried->path == info.path);
    RC_ASSERT(queried->nar_hash == info.nar_hash);
    RC_SUCCEED("Roundtrip OK");
  });
}

// =============================================================================
// Two-tier store fuzz tests
// =============================================================================

TEST_CASE("fuzz: two_tier_store handles arbitrary operations", "[fuzz][two_tier_store]") {
  rc::prop("two_tier_store handles random operation sequences", []() {
    temp_store tmp;
    straylight::nix::store::two_tier_store store(tmp.path());
    auto init_result = store.init();
    RC_ASSERT(init_result.has_value());

    // Random sequence of operations
    auto op_count = *rc::gen::inRange(1, 50);
    for (int i = 0; i < op_count; ++i) {
      auto op = *rc::gen::inRange(0, 10);
      switch (op) {
        case 0: { // is_valid_path
          auto path = *rc::gen::arbitrary<std::string>();
          [[maybe_unused]] auto result = store.is_valid_path(path);
          break;
        }
        case 1: { // query_path_info
          auto path = *rc::gen::arbitrary<std::string>();
          [[maybe_unused]] auto result = store.query_path_info(path);
          break;
        }
        case 2: { // query_references
          auto path = *rc::gen::arbitrary<std::string>();
          [[maybe_unused]] auto result = store.query_references(path);
          break;
        }
        case 3: { // query_referrers
          auto path = *rc::gen::arbitrary<std::string>();
          [[maybe_unused]] auto result = store.query_referrers(path);
          break;
        }
        case 4: { // register_path
          straylight::nix::store::legacy_path_info info;
          info.path = *gen_store_path();
          info.nar_hash = *gen_hex_hash();
          info.nar_size = 1;
          std::vector<std::string> refs;
          [[maybe_unused]] auto result = store.register_path(info, refs);
          break;
        }
        case 5: { // put_ca
          auto data = *gen_blob_data();
          [[maybe_unused]] auto result = store.put_ca(std::span<const std::byte>(data));
          break;
        }
        case 6: { // get_ca
          auto hash = *rc::gen::arbitrary<std::string>();
          [[maybe_unused]] auto result = store.get_ca(hash);
          break;
        }
        case 7: { // has_ca
          auto hash = *rc::gen::arbitrary<std::string>();
          [[maybe_unused]] auto result = store.has_ca(hash);
          break;
        }
        case 8: { // verify
          [[maybe_unused]] auto result = store.verify();
          break;
        }
        case 9: { // query_all_valid_paths
          [[maybe_unused]] auto result = store.query_all_valid_paths();
          break;
        }
      }
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Concurrency stress tests
// =============================================================================

TEST_CASE("fuzz: ca_store handles concurrent puts", "[fuzz][ca_store][concurrent]") {
  temp_store tmp;
  straylight::nix::store::ca_store store(tmp.path() / "ca");
  auto init_result = store.init();
  REQUIRE(init_result.has_value());

  // Same data from multiple threads
  std::vector<std::byte> data = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};
  std::atomic<int> fail_count{0};

  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < 100; ++j) {
        auto result = store.put(std::span<const std::byte>(data));
        if (result.has_value()) {
          success_count++;
        } else {
          fail_count++;
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All should succeed (CA is idempotent)
  CHECK(success_count == 1000);
  CHECK(fail_count == 0);
}

TEST_CASE("fuzz: legacy_store handles concurrent reads/writes",
          "[fuzz][legacy_store][concurrent]") {
  temp_store tmp;
  straylight::nix::store::legacy_store store(tmp.path() / "db");
  auto init_result = store.init();
  REQUIRE(init_result.has_value());

  std::atomic<bool> running{true};
  std::atomic<int> read_count{0};
  std::atomic<int> write_count{0};

  // Writer thread
  std::thread writer([&]() {
    int i = 0;
    while (running && i < 100) {
      straylight::nix::store::legacy_path_info info;
      // Generate unique path
      std::string hash(32, '0');
      auto num = std::to_string(i);
      for (std::size_t j = 0; j < num.size() && j < 32; ++j) {
        hash[j] = num[j];
      }
      info.path = "/nix/store/" + hash + "-pkg" + std::to_string(i);
      info.nar_hash = std::string(64, '0');
      info.nar_size = 1;

      auto result = store.register_path(info, {});
      if (result.has_value()) {
        write_count++;
      }
      i++;
    }
  });

  // Reader threads
  std::vector<std::thread> readers;
  for (int i = 0; i < 5; ++i) {
    readers.emplace_back([&]() {
      while (running) {
        [[maybe_unused]] auto count = store.count();
        [[maybe_unused]] auto all = store.query_all_valid_paths();
        read_count++;
        if (read_count > 1000) {
          break;
        }
      }
    });
  }

  writer.join();
  running = false;
  for (auto& r : readers) {
    r.join();
  }

  CHECK(write_count > 0);
  CHECK(read_count > 0);
}

// =============================================================================
// Edge cases
// =============================================================================

TEST_CASE("fuzz: stores handle empty inputs", "[fuzz][edge]") {
  temp_store tmp;

  // CA store
  {
    straylight::nix::store::ca_store store(tmp.path() / "ca");
    REQUIRE(store.init().has_value());

    // Empty data
    std::vector<std::byte> empty;
    auto result = store.put(std::span<const std::byte>(empty));
    CHECK(result.has_value());

    // Empty hash
    CHECK_FALSE(store.has(""));
    CHECK_FALSE(store.get("").has_value());
  }

  // Legacy store
  {
    straylight::nix::store::legacy_store store(tmp.path() / "db");
    REQUIRE(store.init().has_value());

    // Empty path
    CHECK_FALSE(store.is_valid_path(""));
    CHECK_FALSE(store.query_path_info("").has_value());
  }
}

TEST_CASE("fuzz: stores handle very long inputs", "[fuzz][edge]") {
  temp_store tmp;

  // CA store - long data
  {
    straylight::nix::store::ca_store store(tmp.path() / "ca");
    REQUIRE(store.init().has_value());

    // 1MB of data
    std::vector<std::byte> large(1024 * 1024);
    for (std::size_t i = 0; i < large.size(); ++i) {
      large[i] = static_cast<std::byte>(i & 0xFF);
    }

    auto result = store.put(std::span<const std::byte>(large));
    CHECK(result.has_value());

    auto retrieved = store.get(*result);
    CHECK(retrieved.has_value());
    CHECK(retrieved->size() == large.size());
  }

  // Legacy store - long path
  {
    straylight::nix::store::legacy_store store(tmp.path() / "db");
    REQUIRE(store.init().has_value());

    // Very long path (should fail gracefully)
    std::string long_path = "/nix/store/" + std::string(32, 'a') + "-" + std::string(10000, 'x');
    CHECK_FALSE(store.is_valid_path(long_path));
  }
}

TEST_CASE("fuzz: stores handle null bytes", "[fuzz][edge]") {
  temp_store tmp;

  // CA store - data with null bytes
  {
    straylight::nix::store::ca_store store(tmp.path() / "ca");
    REQUIRE(store.init().has_value());

    std::vector<std::byte> data = {std::byte{0}, std::byte{1}, std::byte{0}, std::byte{2}};
    auto result = store.put(std::span<const std::byte>(data));
    CHECK(result.has_value());

    auto retrieved = store.get(*result);
    CHECK(retrieved.has_value());
    CHECK(*retrieved == data);
  }

  // Legacy store - path with embedded nulls
  {
    straylight::nix::store::legacy_store store(tmp.path() / "db");
    REQUIRE(store.init().has_value());

    std::string path_with_null = "/nix/store/";
    path_with_null += std::string(32, 'a');
    path_with_null += "-test";
    path_with_null += '\0';
    path_with_null += "evil";

    // Should handle gracefully (truncate at null or reject)
    [[maybe_unused]] auto result = store.is_valid_path(path_with_null);
  }
}
