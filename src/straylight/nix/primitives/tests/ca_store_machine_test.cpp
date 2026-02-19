// ca_store_machine_test.cpp
//
// Tests for CA store evring machines

#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include <straylight/evring/evring.h>
#include <straylight/nix/primitives/ca_store.h>
#include <straylight/nix/primitives/ca_store_machine.h>

namespace fs = std::filesystem;
using namespace straylight::nix::primitives;

// ============================================================================
// Test helpers
// ============================================================================

namespace {

std::string g_test_dir;

void ensure_test_dir() {
  if (g_test_dir.empty()) {
    g_test_dir = "/tmp/ca_store_machine_test_" + std::to_string(::getpid());
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

TEST(bulk_ca_has_machine_finds_existing) {
  auto path = unique_test_path("bulk_has");
  ca_store store(path);
  (void)store.init();

  // Put some blobs
  auto hash1 = store.put(make_data("blob one"));
  auto hash2 = store.put(make_data("blob two"));
  auto hash3 = store.put(make_data("blob three"));
  REQUIRE(hash1.has_value());
  REQUIRE(hash2.has_value());
  REQUIRE(hash3.has_value());

  // Check with machine
  std::vector<std::string> hashes = {*hash1, *hash2, *hash3, std::string(64, 'f')};
  std::vector<std::uint8_t> results(hashes.size()); // Use uint8_t, not bool

  auto ring = evring::make_io_uring_ring(64);
  REQUIRE(ring);

  bulk_ca_has_machine machine(path, hashes, evring::make_stable_span(results));
  auto final_state = evring::run_generate(machine, *ring);

  REQUIRE(final_state.finished);
  REQUIRE(results[0] == 1); // exists
  REQUIRE(results[1] == 1); // exists
  REQUIRE(results[2] == 1); // exists
  REQUIRE(results[3] == 0); // does not exist
}

TEST(ca_put_machine_writes_atomically) {
  auto path = unique_test_path("put_machine");
  ca_store store(path);
  (void)store.init();

  auto data = make_data("test content for put machine");

  auto ring = evring::make_io_uring_ring(64);
  REQUIRE(ring);

  ca_put_machine machine(path, data);
  auto expected_hash = machine.hash();

  auto final_state = evring::run(machine, *ring);

  REQUIRE(final_state.current_phase == ca_put_state::phase::done);
  REQUIRE(final_state.hash == expected_hash);

  // Verify we can read it back
  REQUIRE(store.has(expected_hash));
  auto read_result = store.get(expected_hash);
  REQUIRE(read_result.has_value());
  REQUIRE(read_result->size() == data.size());
}

TEST(bulk_ca_read_machine_reads_blobs) {
  auto path = unique_test_path("bulk_read");
  ca_store store(path);
  (void)store.init();

  // Put some blobs
  auto data1 = make_data("first blob content");
  auto data2 = make_data("second blob content");
  auto hash1 = store.put(data1);
  auto hash2 = store.put(data2);
  REQUIRE(hash1.has_value());
  REQUIRE(hash2.has_value());

  std::vector<std::string> hashes = {*hash1, *hash2};
  std::vector<ca_read_result> results(hashes.size());

  auto ring = evring::make_io_uring_ring(64);
  REQUIRE(ring);

  bulk_ca_read_machine machine(path, hashes, evring::make_stable_span(results));
  auto final_state = evring::run_generate(machine, *ring);

  REQUIRE(final_state.current_phase == bulk_ca_read_state::phase::done);
  REQUIRE(results[0].success);
  REQUIRE(results[1].success);
  REQUIRE(results[0].data.size() == data1.size());
  REQUIRE(results[1].data.size() == data2.size());
  REQUIRE(std::memcmp(results[0].data.data(), data1.data(), data1.size()) == 0);
  REQUIRE(std::memcmp(results[1].data.data(), data2.data(), data2.size()) == 0);
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
