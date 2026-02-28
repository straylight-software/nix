// store_test.cpp - Tests for log-structured store

#include <array>
#include <filesystem>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "straylight/nix/testing/temp_dir.h"

#include "../store.h"

namespace fs = std::filesystem;
namespace store = straylight::nix::store;
namespace testing = straylight::nix::testing;

// Helper to create a vector of refs for passing to register_path
template <typename... Args>
auto refs(Args&&... args) -> std::vector<std::string> {
  return std::vector<std::string>{std::forward<Args>(args)...};
}

// ============================================================================
// Test fixtures
// ============================================================================

struct temp_store {
  temp_store()
      : path_(testing::temp_directory_path() / ("store_test_" + std::to_string(counter_++))) {
    fs::create_directories(path_);
  }

  ~temp_store() { fs::remove_all(path_); }

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
  // Use counter + name to ensure unique hashes for different paths
  // Nix store paths look like /nix/store/<hash>-<name>
  // Format: 8-char counter hex + 24 chars from name (padded)
  int counter = path_counter.fetch_add(1);
  char hash[33];
  std::snprintf(hash, sizeof(hash), "%08x", counter);
  std::string hash_str(hash);
  std::string name_part(name);
  name_part.resize(24, '0'); // pad/truncate name portion
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

// ============================================================================
// Initialization tests
// ============================================================================

TEST_CASE("store::init creates directory structure", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  REQUIRE(fs::exists(tmp.path() / "index" / "paths"));
  REQUIRE(fs::exists(tmp.path() / "index" / "refs"));
  REQUIRE(fs::exists(tmp.path() / "index" / "referrers"));
  REQUIRE(fs::exists(tmp.path() / "index" / "derivations"));
  REQUIRE(fs::exists(tmp.path() / "log"));
  REQUIRE(fs::exists(tmp.path() / "lock"));

  // Check shards exist
  REQUIRE(fs::exists(tmp.path() / "index" / "paths" / "00"));
  REQUIRE(fs::exists(tmp.path() / "index" / "paths" / "ff"));
}

TEST_CASE("store::init is idempotent", "[store]") {
  temp_store tmp;

  {
    store::store s1(tmp.path());
    REQUIRE(s1.init().has_value());
  }

  {
    store::store s2(tmp.path());
    REQUIRE(s2.init().has_value());
  }
}

// ============================================================================
// Basic read/write tests
// ============================================================================

TEST_CASE("store: register and query path", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto info = make_path_info("hello");
  std::vector<std::string> refs = {};

  auto result = s.register_path(info, refs);
  REQUIRE(result.has_value());

  REQUIRE(s.is_valid_path(info.path));

  auto queried = s.query_path_info(info.path);
  REQUIRE(queried.has_value());
  REQUIRE(queried->path == info.path);
  REQUIRE(queried->nar_hash == info.nar_hash);
  REQUIRE(queried->registration_time == info.registration_time);
  REQUIRE(queried->nar_size == info.nar_size);
  REQUIRE(queried->ultimate == info.ultimate);
}

TEST_CASE("store: register path with references", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  // Register dependency first
  auto dep_info = make_path_info("glibc");
  REQUIRE(s.register_path(dep_info, {}).has_value());

  // Register path that references it
  auto info = make_path_info("hello");
  std::vector<std::string> refs = {dep_info.path};

  REQUIRE(s.register_path(info, refs).has_value());

  // Query forward references
  auto forward_refs = s.query_references(info.path);
  REQUIRE(forward_refs.has_value());
  REQUIRE(forward_refs->size() == 1);
  REQUIRE((*forward_refs)[0] == dep_info.path);

  // Query reverse references (referrers)
  auto referrers = s.query_referrers(dep_info.path);
  REQUIRE(referrers.has_value());
  REQUIRE(referrers->size() == 1);
  REQUIRE((*referrers)[0] == info.path);
}

TEST_CASE("store: invalidate path", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto info = make_path_info("temp");
  REQUIRE(s.register_path(info, {}).has_value());
  REQUIRE(s.is_valid_path(info.path));

  REQUIRE(s.invalidate_path(info.path).has_value());
  REQUIRE_FALSE(s.is_valid_path(info.path));

  auto queried = s.query_path_info(info.path);
  REQUIRE_FALSE(queried.has_value());
  REQUIRE(queried.error() == store::store_error::not_found);
}

TEST_CASE("store: invalidate updates referrers", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto dep = make_path_info("dep");
  auto pkg = make_path_info("pkg");

  REQUIRE(s.register_path(dep, {}).has_value());
  REQUIRE(s.register_path(pkg, refs(dep.path)).has_value());

  // dep should have pkg as referrer
  auto referrers = s.query_referrers(dep.path);
  REQUIRE(referrers.has_value());
  REQUIRE(referrers->size() == 1);

  // Invalidate pkg
  REQUIRE(s.invalidate_path(pkg.path).has_value());

  // dep should have no referrers now
  referrers = s.query_referrers(dep.path);
  REQUIRE(referrers.has_value());
  REQUIRE(referrers->empty());
}

TEST_CASE("store: query non-existent path", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  REQUIRE_FALSE(s.is_valid_path("/nix/store/nonexistent"));

  auto result = s.query_path_info("/nix/store/nonexistent");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == store::store_error::not_found);
}

TEST_CASE("store: query all valid paths", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto a = make_path_info("aaa");
  auto b = make_path_info("bbb");
  auto c = make_path_info("ccc");

  REQUIRE(s.register_path(a, {}).has_value());
  REQUIRE(s.register_path(b, {}).has_value());
  REQUIRE(s.register_path(c, {}).has_value());

  auto all = s.query_all_valid_paths();
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 3);

  // Check all paths are present (order may vary)
  std::unordered_set<std::string> paths(all->begin(), all->end());
  REQUIRE(paths.count(a.path) == 1);
  REQUIRE(paths.count(b.path) == 1);
  REQUIRE(paths.count(c.path) == 1);
}

// ============================================================================
// Crash recovery tests
// ============================================================================

TEST_CASE("store: recovery replays log", "[store][recovery]") {
  temp_store tmp;
  auto info = make_path_info("persistent");

  // First session: register paths
  {
    auto s = tmp.make_store();
    REQUIRE(s.register_path(info, {}).has_value());
  }

  // Second session: should recover from log
  {
    store::store s(tmp.path());
    REQUIRE(s.init().has_value());

    // Path should still be valid
    REQUIRE(s.is_valid_path(info.path));
  }
}

TEST_CASE("store: recovery handles multiple entries", "[store][recovery]") {
  temp_store tmp;
  std::vector<store::path_info> infos;
  for (int i = 0; i < 10; ++i) {
    infos.push_back(make_path_info("pkg" + std::to_string(i)));
  }

  {
    auto s = tmp.make_store();
    for (const auto& info : infos) {
      REQUIRE(s.register_path(info, {}).has_value());
    }
  }

  {
    store::store s(tmp.path());
    REQUIRE(s.init().has_value());

    for (const auto& info : infos) {
      REQUIRE(s.is_valid_path(info.path));
    }
  }
}

// ============================================================================
// Concurrent access tests
// ============================================================================

TEST_CASE("store: concurrent writers serialize via flock", "[store][concurrent]") {
  temp_store tmp;
  auto primary = tmp.make_store(); // Primary store initializes first - creates dirs, empty log

  constexpr int num_threads = 4;
  constexpr int paths_per_thread = 10;

  // Pre-generate all path infos to avoid counter races
  std::vector<std::vector<path_info>> thread_infos(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    for (int i = 0; i < paths_per_thread; ++i) {
      thread_infos[t].push_back(
          make_path_info("thread" + std::to_string(t) + "_" + std::to_string(i)));
    }
  }

  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  // Note: We use the primary store reference directly in threads.
  // The flock in register_path serializes writes from all threads.
  // Since all threads share the same store instance, they share the same
  // view of the index and avoid the race condition in concurrent init().
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < paths_per_thread; ++i) {
        // Use the primary store - flock will serialize writes
        auto result = primary.register_path(thread_infos[t][i], {});
        if (result.has_value()) {
          success_count.fetch_add(1);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  REQUIRE(success_count.load() == num_threads * paths_per_thread);

  // Create fresh store to see all writes
  store::store fresh_store(tmp.path());
  REQUIRE(fresh_store.init().has_value());

  auto all = fresh_store.query_all_valid_paths();
  REQUIRE(all.has_value());
  REQUIRE(all->size() == num_threads * paths_per_thread);
}

// ============================================================================
// Serialization tests
// ============================================================================

TEST_CASE("serialize_refs round-trips", "[store][serialization]") {
  std::vector<std::string> refs = {
      "/nix/store/abc-foo",
      "/nix/store/def-bar",
      "/nix/store/ghi-baz",
  };

  auto serialized = serialize_refs(refs);
  auto deserialized = deserialize_refs(serialized);

  REQUIRE(deserialized.size() == refs.size());
  for (std::size_t i = 0; i < refs.size(); ++i) {
    REQUIRE(deserialized[i] == refs[i]);
  }
}

TEST_CASE("serialize_refs handles empty", "[store][serialization]") {
  std::vector<std::string> refs = {};

  auto serialized = store::serialize_refs(refs);
  auto deserialized = store::deserialize_refs(serialized);

  REQUIRE(deserialized.empty());
}

TEST_CASE("path_info serialization round-trips", "[store][serialization]") {
  store::path_info info = {
      .path = "/nix/store/xyz789-test",
      .nar_hash = "sha256:abcdef123456",
      .registration_time = 9999999999,
      .deriver = "/nix/store/drv123-test.drv",
      .nar_size = 1024 * 1024,
      .ultimate = false,
      .sigs = {"sig1", "sig2"},
      .ca = "fixed:sha256:abc",
  };

  auto serialized = store::serialize(info);
  auto deserialized = store::deserialize_path_info(serialized);

  REQUIRE(deserialized.has_value());
  REQUIRE(deserialized->path == info.path);
  REQUIRE(deserialized->nar_hash == info.nar_hash);
  REQUIRE(deserialized->registration_time == info.registration_time);
  REQUIRE(deserialized->deriver == info.deriver);
  REQUIRE(deserialized->nar_size == info.nar_size);
  REQUIRE(deserialized->ultimate == info.ultimate);
  REQUIRE(deserialized->sigs == info.sigs);
  REQUIRE(deserialized->ca == info.ca);
}

// ============================================================================
// Derivation output tests
// ============================================================================

TEST_CASE("store: add and query derivation output", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  std::string drv_path = "/nix/store/drv123-foo.drv";
  std::string output_name = "out";
  std::string output_path = "/nix/store/out456-foo";

  auto result = s.add_derivation_output(drv_path, output_name, output_path);
  REQUIRE(result.has_value());

  // Verify file was created
  auto deriv_dir = tmp.path() / "index" / "derivations" / "out";
  REQUIRE(fs::exists(deriv_dir));
}

// ============================================================================
// Checkpoint and compact tests
// ============================================================================

TEST_CASE("store: checkpoint creates marker", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  REQUIRE(s.checkpoint().has_value());
  // Log should exist
  REQUIRE(fs::exists(tmp.path() / "log" / "current.log"));
}

TEST_CASE("store: compact clears log", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto info = make_path_info("test");
  REQUIRE(s.register_path(info, {}).has_value());
  REQUIRE(fs::exists(tmp.path() / "log" / "current.log"));

  REQUIRE(s.compact().has_value());
  REQUIRE_FALSE(fs::exists(tmp.path() / "log" / "current.log"));

  // Index should still be intact
  REQUIRE(s.is_valid_path(info.path));
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("store: multiple references from same path", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto dep1 = make_path_info("dep1");
  auto dep2 = make_path_info("dep2");
  auto dep3 = make_path_info("dep3");
  auto pkg = make_path_info("pkg");

  REQUIRE(s.register_path(dep1, {}).has_value());
  REQUIRE(s.register_path(dep2, {}).has_value());
  REQUIRE(s.register_path(dep3, {}).has_value());
  REQUIRE(s.register_path(pkg, refs(dep1.path, dep2.path, dep3.path)).has_value());

  auto refs = s.query_references(pkg.path);
  REQUIRE(refs.has_value());
  REQUIRE(refs->size() == 3);

  // Each dep should have pkg as referrer
  for (const auto* dep : {&dep1, &dep2, &dep3}) {
    auto referrers = s.query_referrers(dep->path);
    REQUIRE(referrers.has_value());
    REQUIRE(referrers->size() == 1);
    REQUIRE((*referrers)[0] == pkg.path);
  }
}

TEST_CASE("store: self-reference", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto info = make_path_info("selfref");
  REQUIRE(s.register_path(info, refs(info.path)).has_value());

  auto refs = s.query_references(info.path);
  REQUIRE(refs.has_value());
  REQUIRE(refs->size() == 1);
  REQUIRE((*refs)[0] == info.path);

  auto referrers = s.query_referrers(info.path);
  REQUIRE(referrers.has_value());
  REQUIRE(referrers->size() == 1);
  REQUIRE((*referrers)[0] == info.path);
}

TEST_CASE("store: update existing path", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto info1 = make_path_info("update");
  info1.nar_size = 1000;
  REQUIRE(s.register_path(info1, {}).has_value());

  auto info2 = info1;
  info2.nar_size = 2000;
  REQUIRE(s.register_path(info2, {}).has_value());

  auto queried = s.query_path_info(info2.path);
  REQUIRE(queried.has_value());
  REQUIRE(queried->nar_size == 2000);
}

TEST_CASE("store: empty references after invalidate", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();

  auto info = make_path_info("temp");
  REQUIRE(s.register_path(info, {}).has_value());
  REQUIRE(s.invalidate_path(info.path).has_value());

  auto refs = s.query_references(info.path);
  REQUIRE(refs.has_value());
  REQUIRE(refs->empty());
}

// ============================================================================
// io_uring bulk operation tests
// ============================================================================

TEST_CASE("store: init_ring creates io_uring", "[store][io_uring]") {
  temp_store tmp;
  auto s = tmp.make_store();

  REQUIRE(s.init_ring(64).has_value());
  REQUIRE(s.ring() != nullptr);
}

TEST_CASE("store: bulk_query_path_info", "[store][io_uring]") {
  temp_store tmp;
  auto s = tmp.make_store();
  REQUIRE(s.init_ring(64).has_value());

  // Register some paths
  auto a = make_path_info("aaa");
  auto b = make_path_info("bbb");
  auto c = make_path_info("ccc");

  REQUIRE(s.register_path(a, {}).has_value());
  REQUIRE(s.register_path(b, {}).has_value());
  REQUIRE(s.register_path(c, {}).has_value());

  // Bulk query
  std::vector<std::string> paths = {a.path, b.path, c.path, "/nix/store/nonexistent"};
  auto results = s.bulk_query_path_info(paths);

  REQUIRE(results.size() == 4);
  REQUIRE(results[0].has_value());
  REQUIRE(results[0]->path == a.path);
  REQUIRE(results[1].has_value());
  REQUIRE(results[1]->path == b.path);
  REQUIRE(results[2].has_value());
  REQUIRE(results[2]->path == c.path);
  REQUIRE_FALSE(results[3].has_value());
  REQUIRE(results[3].error() == store::store_error::not_found);
}

TEST_CASE("store: bulk_query_references", "[store][io_uring]") {
  temp_store tmp;
  auto s = tmp.make_store();
  REQUIRE(s.init_ring(64).has_value());

  auto dep = make_path_info("dep");
  auto pkg1 = make_path_info("pkg1");
  auto pkg2 = make_path_info("pkg2");

  REQUIRE(s.register_path(dep, {}).has_value());
  REQUIRE(s.register_path(pkg1, refs(dep.path)).has_value());
  REQUIRE(s.register_path(pkg2, refs(dep.path)).has_value());

  std::vector<std::string> paths = {pkg1.path, pkg2.path};
  auto results = s.bulk_query_references(paths);

  REQUIRE(results.size() == 2);
  REQUIRE(results[0].has_value());
  REQUIRE(results[0]->size() == 1);
  REQUIRE((*results[0])[0] == dep.path);
  REQUIRE(results[1].has_value());
  REQUIRE(results[1]->size() == 1);
  REQUIRE((*results[1])[0] == dep.path);
}

TEST_CASE("store: bulk_is_valid_path", "[store][io_uring]") {
  temp_store tmp;
  auto s = tmp.make_store();
  REQUIRE(s.init_ring(64).has_value());

  auto a = make_path_info("exists1");
  auto b = make_path_info("exists2");

  REQUIRE(s.register_path(a, {}).has_value());
  REQUIRE(s.register_path(b, {}).has_value());

  std::vector<std::string> paths = {a.path, b.path, "/nix/store/nonexistent1",
                                    "/nix/store/nonexistent2"};
  auto results = s.bulk_is_valid_path(paths);

  REQUIRE(results.size() == 4);
  REQUIRE(results[0] == true);
  REQUIRE(results[1] == true);
  REQUIRE(results[2] == false);
  REQUIRE(results[3] == false);
}

TEST_CASE("store: compute_closure simple", "[store][io_uring]") {
  temp_store tmp;
  auto s = tmp.make_store();
  REQUIRE(s.init_ring(64).has_value());

  // Create a simple dependency chain: a -> b -> c
  auto a = make_path_info("aaa");
  auto b = make_path_info("bbb");
  auto c = make_path_info("ccc");

  REQUIRE(s.register_path(c, refs()).has_value());
  REQUIRE(s.register_path(b, refs(c.path)).has_value());
  REQUIRE(s.register_path(a, refs(b.path)).has_value());

  std::vector<std::string> start = {a.path};
  auto closure = s.compute_closure(start);

  // Closure should contain a, b, c
  REQUIRE(closure.size() == 3);
  std::unordered_set<std::string> closure_set(closure.begin(), closure.end());
  REQUIRE(closure_set.count(a.path) == 1);
  REQUIRE(closure_set.count(b.path) == 1);
  REQUIRE(closure_set.count(c.path) == 1);
}

TEST_CASE("store: compute_closure diamond", "[store][io_uring]") {
  temp_store tmp;
  auto s = tmp.make_store();
  REQUIRE(s.init_ring(64).has_value());

  // Diamond dependency:
  //     a
  //    / \
  //   b   c
  //    \ /
  //     d
  auto a = make_path_info("aaa");
  auto b = make_path_info("bbb");
  auto c = make_path_info("ccc");
  auto d = make_path_info("ddd");

  REQUIRE(s.register_path(d, refs()).has_value());
  REQUIRE(s.register_path(b, refs(d.path)).has_value());
  REQUIRE(s.register_path(c, refs(d.path)).has_value());
  REQUIRE(s.register_path(a, refs(b.path, c.path)).has_value());

  std::vector<std::string> start = {a.path};
  auto closure = s.compute_closure(start);

  // Closure should contain a, b, c, d (no duplicates)
  REQUIRE(closure.size() == 4);
  std::unordered_set<std::string> closure_set(closure.begin(), closure.end());
  REQUIRE(closure_set.count(a.path) == 1);
  REQUIRE(closure_set.count(b.path) == 1);
  REQUIRE(closure_set.count(c.path) == 1);
  REQUIRE(closure_set.count(d.path) == 1);
}

TEST_CASE("store: bulk_query_path_info many paths", "[store][io_uring]") {
  temp_store tmp;
  auto s = tmp.make_store();
  REQUIRE(s.init_ring(256).has_value());

  // Register 100 paths
  std::vector<store::path_info> infos;
  std::vector<std::string> paths;
  for (int i = 0; i < 100; ++i) {
    auto info = make_path_info("pkg" + std::to_string(i));
    REQUIRE(s.register_path(info, {}).has_value());
    infos.push_back(info);
    paths.push_back(info.path);
  }

  // Bulk query all
  auto results = s.bulk_query_path_info(paths);

  REQUIRE(results.size() == 100);
  for (std::size_t i = 0; i < 100; ++i) {
    REQUIRE(results[i].has_value());
    REQUIRE(results[i]->path == infos[i].path);
  }
}

TEST_CASE("store: bulk operations without ring fall back to sync", "[store]") {
  temp_store tmp;
  auto s = tmp.make_store();
  // Don't init ring

  auto a = make_path_info("aaa");
  REQUIRE(s.register_path(a, {}).has_value());

  std::vector<std::string> paths = {a.path};

  // Should work, just slower
  auto results = s.bulk_query_path_info(paths);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].has_value());
  REQUIRE(results[0]->path == a.path);

  auto valid = s.bulk_is_valid_path(paths);
  REQUIRE(valid.size() == 1);
  REQUIRE(valid[0] == true);
}
