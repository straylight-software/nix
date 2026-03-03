// straylight::nix::adapters::tests
//
// Tests for the store_adapter - verifies the two_tier_store integration
// through the nix store_t interface.

#include <atomic>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <straylight/nix/store/two_tier_store.h>
#include <straylight/nix/testing/temp_dir.h>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

// Use fully qualified names to avoid shadowing
using straylight::nix::store::legacy_path_info;
using straylight::nix::store::store_tier_error;
using straylight::nix::store::two_tier_store;

// ─────────────────────────────────────────────────────────────────────────────
// Test fixtures
// ─────────────────────────────────────────────────────────────────────────────

struct temp_store_dir {
  temp_store_dir()
      : path_(straylight::nix::testing::temp_directory_path() /
              ("store_adapter_test_" + std::to_string(counter_++))) {
    fs::create_directories(path_);
  }

  ~temp_store_dir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  [[nodiscard]] auto path() const -> const fs::path& { return path_; }

  [[nodiscard]] auto make_two_tier_store() -> two_tier_store {
    two_tier_store s(path_);
    auto result = s.init();
    REQUIRE(result.has_value());
    return s;
  }

  fs::path path_;
  static inline int counter_ = 0;
};

static inline std::atomic<int> path_counter{0};

static auto make_test_path_info(std::string_view name) -> legacy_path_info {
  int counter = path_counter.fetch_add(1);
  char hash[33];
  std::snprintf(hash, sizeof(hash), "%08x", counter);
  std::string hash_str(hash);
  std::string name_part(name);
  name_part.resize(24, '0');
  hash_str += name_part;

  return legacy_path_info{
      .path = "/nix/store/" + hash_str + "-" + std::string(name),
      .nar_hash = "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      .registration_time = 1234567890,
      .deriver = "",
      .nar_size = 1024,
      .ultimate = true,
      .sigs = {},
      .ca = "",
  };
}

// ─────────────────────────────────────────────────────────────────────────────
// two_tier_store basic operations (underlying store for adapter)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("two_tier_store initialization", "[store_adapter][two_tier]") {
  temp_store_dir tmp;
  auto store = tmp.make_two_tier_store();

  SECTION("directories are created") {
    REQUIRE(fs::exists(tmp.path() / "ca"));
    REQUIRE(fs::exists(tmp.path() / "db"));
  }
}

TEST_CASE("two_tier_store path registration", "[store_adapter][two_tier]") {
  temp_store_dir tmp;
  auto store = tmp.make_two_tier_store();

  SECTION("register and query single path") {
    auto info = make_test_path_info("hello");

    auto reg_result = store.register_path(info, {});
    REQUIRE(reg_result.has_value());

    REQUIRE(store.is_valid_path(info.path));

    auto query_result = store.query_path_info(info.path);
    REQUIRE(query_result.has_value());
    REQUIRE(query_result->path == info.path);
    REQUIRE(query_result->nar_hash == info.nar_hash);
    REQUIRE(query_result->nar_size == info.nar_size);
  }

  SECTION("register path with references") {
    auto dep_info = make_test_path_info("dependency");
    auto pkg_info = make_test_path_info("package");

    // Register dependency first
    auto dep_result = store.register_path(dep_info, {});
    REQUIRE(dep_result.has_value());

    // Register package with reference to dependency
    std::vector<std::string> refs = {dep_info.path};
    auto pkg_result = store.register_path(pkg_info, refs);
    REQUIRE(pkg_result.has_value());

    // Query references
    auto refs_result = store.query_references(pkg_info.path);
    REQUIRE(refs_result.has_value());
    REQUIRE(refs_result->size() == 1);
    REQUIRE((*refs_result)[0] == dep_info.path);

    // Query referrers
    auto referrers_result = store.query_referrers(dep_info.path);
    REQUIRE(referrers_result.has_value());
    REQUIRE(referrers_result->size() == 1);
    REQUIRE((*referrers_result)[0] == pkg_info.path);
  }

  SECTION("query non-existent path") {
    REQUIRE_FALSE(store.is_valid_path("/nix/store/nonexistent-path"));

    auto result = store.query_path_info("/nix/store/nonexistent-path");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == store_tier_error::not_found);
  }
}

TEST_CASE("two_tier_store query_all_valid_paths", "[store_adapter][two_tier]") {
  temp_store_dir tmp;
  auto store = tmp.make_two_tier_store();

  // Register several paths
  auto info1 = make_test_path_info("pkg1");
  auto info2 = make_test_path_info("pkg2");
  auto info3 = make_test_path_info("pkg3");

  REQUIRE(store.register_path(info1, {}).has_value());
  REQUIRE(store.register_path(info2, {}).has_value());
  REQUIRE(store.register_path(info3, {}).has_value());

  auto all_paths = store.query_all_valid_paths();
  REQUIRE(all_paths.has_value());
  REQUIRE(all_paths->size() == 3);

  // Check all paths are present
  std::set<std::string> path_set(all_paths->begin(), all_paths->end());
  REQUIRE(path_set.count(info1.path) == 1);
  REQUIRE(path_set.count(info2.path) == 1);
  REQUIRE(path_set.count(info3.path) == 1);
}

TEST_CASE("two_tier_store derivation outputs", "[store_adapter][two_tier]") {
  temp_store_dir tmp;
  auto store = tmp.make_two_tier_store();

  SECTION("add and query derivation output") {
    // First register a derivation path (required before adding outputs)
    auto drv_info = make_test_path_info("mypackage.drv");
    REQUIRE(store.register_path(drv_info, {}).has_value());

    std::string output_name = "out";
    std::string output_path = "/nix/store/xyz789-result";

    // Now we can add the derivation output
    auto add_result = store.add_derivation_output(drv_info.path, output_name, output_path);
    REQUIRE(add_result.has_value());

    auto query_result = store.query_derivation_output(drv_info.path, output_name);
    REQUIRE(query_result.has_value());
    REQUIRE(*query_result == output_path);
  }

  SECTION("query non-existent derivation output") {
    auto result = store.query_derivation_output("/nix/store/nonexistent-drv", "out");
    REQUIRE_FALSE(result.has_value());
  }
}

TEST_CASE("two_tier_store path invalidation", "[store_adapter][two_tier]") {
  temp_store_dir tmp;
  auto store = tmp.make_two_tier_store();

  auto info = make_test_path_info("to-delete");
  REQUIRE(store.register_path(info, {}).has_value());
  REQUIRE(store.is_valid_path(info.path));

  auto invalidate_result = store.invalidate_path(info.path);
  REQUIRE(invalidate_result.has_value());
  REQUIRE_FALSE(store.is_valid_path(info.path));
}

TEST_CASE("two_tier_store CA operations", "[store_adapter][two_tier]") {
  temp_store_dir tmp;
  auto store = tmp.make_two_tier_store();

  std::vector<std::byte> test_data = {std::byte{0x48}, std::byte{0x65}, std::byte{0x6c},
                                      std::byte{0x6c}, std::byte{0x6f}}; // "Hello"

  SECTION("put and get CA content") {
    auto put_result = store.put_ca(test_data);
    REQUIRE(put_result.has_value());

    std::string hash = *put_result;
    REQUIRE_FALSE(hash.empty());
    REQUIRE(store.has_ca(hash));

    auto get_result = store.get_ca(hash);
    REQUIRE(get_result.has_value());
    REQUIRE(*get_result == test_data);
  }

  SECTION("has_ca returns false for non-existent") {
    REQUIRE_FALSE(store.has_ca("sha256:nonexistent"));
  }
}

TEST_CASE("two_tier_store verify", "[store_adapter][two_tier]") {
  temp_store_dir tmp;
  auto store = tmp.make_two_tier_store();

  // Fresh store should verify clean
  auto result = store.verify();
  REQUIRE(result.has_value());
  REQUIRE(*result == true); // true = no errors
}

TEST_CASE("two_tier_store vacuum", "[store_adapter][two_tier]") {
  temp_store_dir tmp;
  auto store = tmp.make_two_tier_store();

  // Register and invalidate some paths to create garbage
  auto info = make_test_path_info("garbage");
  REQUIRE(store.register_path(info, {}).has_value());
  REQUIRE(store.invalidate_path(info.path).has_value());

  // Vacuum should succeed
  auto result = store.vacuum();
  REQUIRE(result.has_value());
}
