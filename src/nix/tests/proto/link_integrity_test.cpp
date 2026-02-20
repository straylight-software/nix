// straylight // nix // tests // proto
//
// Protological Link Integrity Tests
//
// These tests verify that all declared symbols are actually defined.
// If this file compiles and links, the symbol table is consistent.
// Any missing implementation will cause a link error, not a runtime error.

#include <catch2/catch_test_macros.hpp>

#include "nix/tests/property.h"

// =============================================================================
// util module headers
// =============================================================================

#include "nix/util/archive.h"
#include "nix/util/canon-path.h"
#include "nix/util/compression.h"
#include "nix/util/error.h"
#include "nix/util/experimental-features.h"
#include "nix/util/hash.h"
#include "nix/util/json-utils.h"
#include "nix/util/logging.h"
#include "nix/util/lru-cache.h"
#include "nix/util/memory-source-accessor.h"
#include "nix/util/nar-accessor.h"
#include "nix/util/pool.h"
#include "nix/util/serialise.h"
#include "nix/util/source-accessor.h"
#include "nix/util/source-path.h"
#include "nix/util/strings.h"
#include "nix/util/sync.h"
#include "nix/util/url.h"

// =============================================================================
// store module headers
// =============================================================================

#include "nix/store/content-address.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/path-info.h"
#include "nix/store/path.h"
#include "nix/store/store-api.h"
#include "nix/store/store-dir-config.h"

// =============================================================================
// fetchers module headers
// =============================================================================

#include "nix/fetchers/fetchers.h"

// =============================================================================
// expr module headers
// =============================================================================

#include "nix/expr/eval.h"
#include "nix/expr/nixexpr.h"
#include "nix/expr/symbol-table.h"
#include "nix/expr/value.h"

// =============================================================================
// flake module headers
// =============================================================================

#include "nix/flake/flake.h"
#include "nix/flake/flakeref.h"
#include "nix/flake/lockfile.h"

using namespace nix;

// =============================================================================
// Template instantiation verification
// Force instantiation of all template types used in the codebase
// =============================================================================

namespace {

template <typename T>
void force_instantiation() {
  [[maybe_unused]] volatile auto size = sizeof(T);
}

void instantiate_fso_templates() {
  force_instantiation<fso::variant_t<std::string, true>>();
  force_instantiation<fso::variant_t<std::string, false>>();
  force_instantiation<fso::variant_t<nar_listing_regular_file_t, true>>();
  force_instantiation<fso::variant_t<nar_listing_regular_file_t, false>>();
  force_instantiation<fso::regular<std::string>>();
  force_instantiation<fso::regular<nar_listing_regular_file_t>>();
  force_instantiation<fso::directory_t<fso::variant_t<std::string, true>>>();
  force_instantiation<fso::directory_t<fso::opaque_t>>();
}

void instantiate_json_serializers() {
  using json = nlohmann::json;
  {
    nar_listing_t listing;
    json j = listing;
    [[maybe_unused]] auto back = j.get<nar_listing_t>();
  }
  {
    shallow_nar_listing_t listing;
    json j = listing;
    [[maybe_unused]] auto back = j.get<shallow_nar_listing_t>();
  }
  {
    memory_source_accessor_t::file_t file;
    json j = file;
    [[maybe_unused]] auto back = j.get<memory_source_accessor_t::file_t>();
  }
}

void instantiate_lru_cache() {
  force_instantiation<lru_cache_t<std::string, int>>();
  force_instantiation<lru_cache_t<hash_t, std::string>>();
}

void instantiate_pool() {
  force_instantiation<pool_t<std::string>>();
}

void instantiate_sync() {
  force_instantiation<sync_t<int>>();
  force_instantiation<sync_t<std::string>>();
  force_instantiation<sync_t<std::vector<int>>>();
}

} // anonymous namespace

// =============================================================================
// Link integrity tests - verify symbols are linked
// =============================================================================

TEST_CASE("link integrity: all headers compile", "[proto][link]") {
  REQUIRE(true);
}

TEST_CASE("link integrity: template instantiations", "[proto][link]") {
  REQUIRE_NOTHROW(instantiate_fso_templates());
  REQUIRE_NOTHROW(instantiate_lru_cache());
  REQUIRE_NOTHROW(instantiate_pool());
  REQUIRE_NOTHROW(instantiate_sync());
}

TEST_CASE("link integrity: json serializers", "[proto][link]") {
  REQUIRE_NOTHROW(instantiate_json_serializers());
}

// =============================================================================
// Hash function link verification
// =============================================================================

TEST_CASE("link integrity: hash functions", "[proto][link]") {
  auto h = hash_string(hash_algorithm_t::sha256, "test");
  REQUIRE(h.hash_size() == 32);
  REQUIRE(h.algo() == hash_algorithm_t::sha256);

  auto hex = h.to_string(hash_format_t::base16, false);
  REQUIRE(hex.size() == 64);

  auto sri = h.to_string(hash_format_t::sri, true);
  REQUIRE(sri.starts_with("sha256-"));
}

TEST_CASE("link integrity: all hash algorithms", "[proto][link]") {
  for (auto algo : {hash_algorithm_t::md5, hash_algorithm_t::sha1, hash_algorithm_t::sha256,
                    hash_algorithm_t::sha512}) {
    auto h = hash_string(algo, "test data");
    REQUIRE(h.hash_size() == regular_hash_size(algo));
    REQUIRE(h.algo() == algo);
  }
}

// =============================================================================
// Path link verification
// =============================================================================

TEST_CASE("link integrity: canon_path_t", "[proto][link]") {
  canon_path_t p("/foo/bar");
  REQUIRE(p.abs() == "/foo/bar");
  REQUIRE(p.base_name() == "bar");
  REQUIRE(p.dir_of().has_value());
  REQUIRE(p.dir_of().value() == "/foo");
}

// =============================================================================
// Compression link verification
// =============================================================================

TEST_CASE("link integrity: compression", "[proto][link]") {
  std::string data = "test data for compression";
  auto compressed = compress("none", data);
  REQUIRE(compressed == data);
  auto decompressed = decompress("none", compressed);
  REQUIRE(decompressed == data);
}

// =============================================================================
// Store path link verification
// =============================================================================

TEST_CASE("link integrity: store types", "[proto][link]") {
  REQUIRE(sizeof(store_dir_config_t) > 0);
  REQUIRE(sizeof(derivation_t) > 0);
  REQUIRE(sizeof(basic_derivation_t) > 0);
  REQUIRE(sizeof(derivation_output_t) > 0);
  REQUIRE(sizeof(content_address_method_t) > 0);
  REQUIRE(sizeof(content_address_t) > 0);
}

// =============================================================================
// Serialization link verification
// =============================================================================

TEST_CASE("link integrity: serialization", "[proto][link]") {
  // Test raw byte writing via operator()
  string_sink_t sink;
  std::string_view hello = "hello";
  sink(hello);
  REQUIRE(sink.str().size() == 5);
  REQUIRE(sink.str() == "hello");

  // Test string_source_t reading
  std::string data = "test";
  string_source_t source(data);
  char buf[4];
  source(buf, 4);
  REQUIRE(std::string(buf, 4) == "test");
}

// =============================================================================
// Expr/eval link verification
// =============================================================================

TEST_CASE("link integrity: expr types", "[proto][link]") {
  REQUIRE(sizeof(symbol_table_t) > 0);
  REQUIRE(sizeof(pos_table_t) > 0);
  REQUIRE(sizeof(value_t) > 0);
  REQUIRE(sizeof(bindings_t) > 0);
}

// =============================================================================
// Flake link verification
// =============================================================================

TEST_CASE("link integrity: flake types", "[proto][link]") {
  REQUIRE(sizeof(flake_ref_t) > 0);
  REQUIRE(sizeof(flake::lock_file_t) > 0);
}

// =============================================================================
// Property tests for link integrity with data
// =============================================================================

TEST_CASE("link integrity: hash property roundtrip", "[proto][link][property]") {
  rc::prop("hash serialization roundtrip", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto hash = hash_string(hash_algorithm_t::sha256, data);
    auto encoded = hash.to_string(hash_format_t::base16, true);
    auto parsed = hash_t::parse_any_prefixed(encoded);
    RC_ASSERT(hash == parsed);
  });
}

TEST_CASE("link integrity: canon_path property", "[proto][link][property]") {
  rc::prop("canon_path preserves absolute path semantics", []() {
    auto components = *rc::gen::container<std::vector<std::string>>(
        rc::gen::suchThat(rc::gen::string<std::string>(), [](const std::string& s) {
          return !s.empty() && s.find('/') == std::string::npos &&
                 s.find('\0') == std::string::npos && s != "." && s != "..";
        }));

    if (components.empty()) {
      return;
    }

    std::string path = "/";
    for (const auto& c : components) {
      path += c + "/";
    }
    path.pop_back();

    canon_path_t cp(path);
    RC_ASSERT(cp.abs() == path);
    RC_ASSERT(cp.is_root() == (path == "/"));
  });
}
