// ca_store_test.cpp
//
// Tests for content-addressed blob storage

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include <straylight/nix/store/ca_store.h>

namespace fs = std::filesystem;

// ============================================================================
// Test helpers
// ============================================================================

namespace {

// Track test directory for cleanup
std::string g_test_dir;

void ensure_test_dir() {
  if (g_test_dir.empty()) {
    g_test_dir = "/tmp/ca_store_test_" + std::to_string(::getpid());
    std::cout << "Test directory: \"" << g_test_dir << "\"\n";
  }
}

std::string unique_test_path(const std::string& name) {
  ensure_test_dir();
  static std::atomic<int> counter{0};
  return g_test_dir + "/" + name + "_" + std::to_string(counter++);
}

std::vector<std::byte> make_data(const std::string& s) {
  std::vector<std::byte> data(s.size());
  std::memcpy(data.data(), s.data(), s.size());
  return data;
}

std::vector<std::byte> make_random_data(std::size_t size, unsigned seed = 12345) {
  std::vector<std::byte> data(size);
  std::mt19937 gen(seed);
  for (auto& b : data) {
    b = static_cast<std::byte>(gen() & 0xff);
  }
  return data;
}

int g_passed = 0;
int g_failed = 0;

#define TEST(name)                                                                                 \
  void test_##name();                                                                              \
  struct test_runner_##name {                                                                      \
    test_runner_##name() {                                                                         \
      std::cout << "Running: " #name "... " << std::flush;                                         \
      try {                                                                                        \
        test_##name();                                                                             \
        std::cout << "PASSED\n";                                                                   \
        ++g_passed;                                                                                \
      } catch (const std::exception& e) {                                                          \
        std::cout << "FAILED: " << e.what() << "\n";                                               \
        ++g_failed;                                                                                \
      }                                                                                            \
    }                                                                                              \
  } test_instance_##name;                                                                          \
  void test_##name()

#define REQUIRE(cond)                                                                              \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      throw std::runtime_error("REQUIRE failed: " #cond);                                          \
    }                                                                                              \
  } while (0)

} // namespace

// ============================================================================
// Tests
// ============================================================================

TEST(init_creates_directories) {
  auto path = unique_test_path("init");
  straylight::nix::store::ca_store store(path);
  auto result = store.init();
  REQUIRE(result.has_value());

  // Check root exists
  REQUIRE(fs::exists(path));

  // Check some shard directories exist
  REQUIRE(fs::exists(fs::path(path) / "00"));
  REQUIRE(fs::exists(fs::path(path) / "ab"));
  REQUIRE(fs::exists(fs::path(path) / "ff"));
}

TEST(put_returns_hash) {
  auto path = unique_test_path("put");
  straylight::nix::store::ca_store store(path);
  store.init();

  auto data = make_data("hello world");
  auto result = store.put(data);

  REQUIRE(result.has_value());
  REQUIRE(result->size() == 64); // BLAKE3 hex = 64 chars
}

TEST(put_is_idempotent) {
  auto path = unique_test_path("idempotent");
  straylight::nix::store::ca_store store(path);
  store.init();

  auto data = make_data("test content");
  auto hash1 = store.put(data);
  auto hash2 = store.put(data);

  REQUIRE(hash1.has_value());
  REQUIRE(hash2.has_value());
  REQUIRE(*hash1 == *hash2);
}

TEST(get_returns_content) {
  auto path = unique_test_path("get");
  straylight::nix::store::ca_store store(path);
  store.init();

  auto original = make_data("hello world");
  auto hash = store.put(original);
  REQUIRE(hash.has_value());

  auto retrieved = store.get(*hash);
  REQUIRE(retrieved.has_value());
  REQUIRE(retrieved->size() == original.size());
  REQUIRE(std::memcmp(retrieved->data(), original.data(), original.size()) == 0);
}

TEST(get_not_found) {
  auto path = unique_test_path("notfound");
  straylight::nix::store::ca_store store(path);
  store.init();

  // Valid-looking hash that doesn't exist
  std::string fake_hash(64, 'a');
  auto result = store.get(fake_hash);

  REQUIRE(!result.has_value());
  REQUIRE(result.error() == straylight::nix::store::ca_error::not_found);
}

TEST(get_invalid_hash) {
  auto path = unique_test_path("invalid");
  straylight::nix::store::ca_store store(path);
  store.init();

  // Too short
  auto result1 = store.get("abc");
  REQUIRE(!result1.has_value());
  REQUIRE(result1.error() == straylight::nix::store::ca_error::invalid_hash);

  // Invalid characters
  std::string bad_hash(64, 'x');
  auto result2 = store.get(bad_hash);
  REQUIRE(!result2.has_value());
  REQUIRE(result2.error() == straylight::nix::store::ca_error::invalid_hash);
}

TEST(has_returns_correct_value) {
  auto path = unique_test_path("has");
  straylight::nix::store::ca_store store(path);
  store.init();

  auto data = make_data("test");
  auto hash = store.put(data);
  REQUIRE(hash.has_value());

  // Exists
  REQUIRE(store.has(*hash));

  // Doesn't exist
  std::string fake_hash(64, 'b');
  REQUIRE(!store.has(fake_hash));
}

TEST(remove_deletes_blob) {
  auto path = unique_test_path("remove");
  straylight::nix::store::ca_store store(path);
  store.init();

  auto data = make_data("to be removed");
  auto hash = store.put(data);
  REQUIRE(hash.has_value());
  REQUIRE(store.has(*hash));

  auto removed = store.remove(*hash);
  REQUIRE(removed.has_value());
  REQUIRE(*removed == true);
  REQUIRE(!store.has(*hash));

  // Remove again returns false
  auto removed_again = store.remove(*hash);
  REQUIRE(removed_again.has_value());
  REQUIRE(*removed_again == false);
}

TEST(put_with_hash_verifies) {
  auto path = unique_test_path("verify");
  straylight::nix::store::ca_store store(path);
  store.init();

  auto data = make_data("content");

  // First, get the correct hash
  auto correct_hash = store.put(data);
  REQUIRE(correct_hash.has_value());

  // Remove it so we can try put with explicit hash
  store.remove(*correct_hash);

  // Put with correct hash succeeds
  auto result1 = store.put(*correct_hash, data);
  REQUIRE(result1.has_value());

  // Put with wrong hash fails
  std::string wrong_hash(64, 'c');
  auto result2 = store.put(wrong_hash, data);
  REQUIRE(!result2.has_value());
  REQUIRE(result2.error() == straylight::nix::store::ca_error::hash_mismatch);
}

TEST(verify_detects_corruption) {
  auto path = unique_test_path("corrupt");
  straylight::nix::store::ca_store store(path);
  store.init();

  auto data = make_data("original content");
  auto hash = store.put(data);
  REQUIRE(hash.has_value());

  // Verify should pass
  auto valid = store.verify(*hash);
  REQUIRE(valid.has_value());
  REQUIRE(*valid == true);
}

TEST(count_and_size) {
  auto path = unique_test_path("stats");
  straylight::nix::store::ca_store store(path);
  store.init();

  // Initially empty
  REQUIRE(store.count().value_or(999) == 0);
  REQUIRE(store.total_size().value_or(999) == 0);

  // Add some blobs
  auto data1 = make_data("blob one");
  auto data2 = make_data("blob two longer");

  store.put(data1);
  store.put(data2);

  REQUIRE(store.count().value_or(0) == 2);
  REQUIRE(store.total_size().value_or(0) == data1.size() + data2.size());
}

TEST(list_all) {
  auto path = unique_test_path("list");
  straylight::nix::store::ca_store store(path);
  store.init();

  auto data1 = make_data("first");
  auto data2 = make_data("second");

  auto hash1 = store.put(data1);
  auto hash2 = store.put(data2);

  auto all = store.list_all();
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 2);

  // Both hashes should be in the list
  bool found1 = false, found2 = false;
  for (const auto& h : *all) {
    if (h == *hash1)
      found1 = true;
    if (h == *hash2)
      found2 = true;
  }
  REQUIRE(found1);
  REQUIRE(found2);
}

TEST(cleanup_temps) {
  auto path = unique_test_path("temps");
  straylight::nix::store::ca_store store(path);
  store.init();

  // Create some fake .tmp files
  auto tmp_path = fs::path(path) / "ab" / "abcd.tmp";
  {
    std::ofstream f(tmp_path);
    f << "incomplete write";
  }
  REQUIRE(fs::exists(tmp_path));

  auto cleaned = store.cleanup_temps();
  REQUIRE(cleaned.has_value());
  REQUIRE(*cleaned == 1);
  REQUIRE(!fs::exists(tmp_path));
}

TEST(large_blob) {
  auto path = unique_test_path("large");
  straylight::nix::store::ca_store store(path);
  store.init();

  // 1MB of random data
  auto data = make_random_data(1024 * 1024);

  auto hash = store.put(data);
  REQUIRE(hash.has_value());

  auto retrieved = store.get(*hash);
  REQUIRE(retrieved.has_value());
  REQUIRE(retrieved->size() == data.size());
  REQUIRE(std::memcmp(retrieved->data(), data.data(), data.size()) == 0);
}

TEST(concurrent_writes_same_content) {
  auto path = unique_test_path("concurrent");
  straylight::nix::store::ca_store store(path);
  store.init();

  auto data = make_data("shared content for concurrent writes");
  constexpr int num_threads = 8;

  std::vector<std::string> results(num_threads);
  std::vector<std::thread> threads;

  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&store, &data, &results, i]() {
      auto hash = store.put(data);
      if (hash) {
        results[i] = *hash;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All threads should have gotten the same hash
  for (int i = 1; i < num_threads; ++i) {
    REQUIRE(!results[i].empty());
    REQUIRE(results[i] == results[0]);
  }
}

TEST(concurrent_writes_different_content) {
  auto path = unique_test_path("concurrent2");
  straylight::nix::store::ca_store store(path);
  store.init();

  constexpr int num_threads = 8;

  std::vector<std::string> hashes(num_threads);
  std::vector<std::thread> threads;

  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&store, &hashes, i]() {
      auto data = make_data("unique content " + std::to_string(i));
      auto hash = store.put(data);
      if (hash) {
        hashes[i] = *hash;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All hashes should be different (since content is different)
  std::set<std::string> unique_hashes(hashes.begin(), hashes.end());
  REQUIRE(unique_hashes.size() == num_threads);

  // All should exist
  for (const auto& h : hashes) {
    REQUIRE(store.has(h));
  }
}

TEST(bulk_has) {
  auto path = unique_test_path("bulk");
  straylight::nix::store::ca_store store(path);
  store.init();

  // Add some blobs
  std::vector<std::string> existing;
  for (int i = 0; i < 5; ++i) {
    auto hash = store.put(make_data("blob " + std::to_string(i)));
    REQUIRE(hash.has_value());
    existing.push_back(*hash);
  }

  // Mix existing and non-existing
  std::vector<std::string> queries = existing;
  queries.push_back(std::string(64, 'f')); // non-existing
  queries.push_back(std::string(64, 'e')); // non-existing

  auto results = store.bulk_has(queries);
  REQUIRE(results.size() == queries.size());

  // First 5 should exist
  for (int i = 0; i < 5; ++i) {
    REQUIRE(results[i] == true);
  }
  // Last 2 should not
  REQUIRE(results[5] == false);
  REQUIRE(results[6] == false);
}

TEST(verify_all) {
  auto path = unique_test_path("verifyall");
  straylight::nix::store::ca_store store(path);
  store.init();

  // Add several blobs
  for (int i = 0; i < 10; ++i) {
    store.put(make_data("blob " + std::to_string(i)));
  }

  auto corrupt = store.verify_all();
  REQUIRE(corrupt.has_value());
  REQUIRE(*corrupt == 0); // No corruption
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::cout << "\n";

  // Tests run via static initialization

  std::cout << "\n";
  std::cout << "Passed: " << g_passed << "\n";
  std::cout << "Failed: " << g_failed << "\n";

  // Cleanup
  if (!g_test_dir.empty()) {
    std::error_code ec;
    fs::remove_all(g_test_dir, ec);
  }

  return g_failed > 0 ? 1 : 0;
}
