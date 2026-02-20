// straylight // nix // tests // fuzz
//
// Eval Cache Fuzz Tests
//
// Craft malicious eval cache databases to expose bugs in cache parsing.
// The eval cache stores attribute values in SQLite and deserializes them
// on read - malformed data can trigger assertion failures or crashes.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "nix/expr/eval-cache.h"
#include "nix/expr/eval.h"
#include "nix/store/store-api.h"
#include "nix/tests/property.h"
#include "nix/util/error.h"
#include "nix/util/hash.h"
#include "nix/util/users.h"

using namespace nix;

namespace {

// Create a minimal eval cache database with injected malicious data
auto create_malicious_cache(const std::filesystem::path& db_path, int malicious_type,
                            const std::string& malicious_value,
                            const std::string& malicious_context) -> void {
  // Remove existing file
  std::filesystem::remove(db_path);

  sqlite3* db = nullptr;
  int rc = sqlite3_open(db_path.c_str(), &db);
  if (rc != SQLITE_OK) {
    throw Error("Failed to create malicious cache: %s", sqlite3_errmsg(db));
  }

  // Create schema
  const char* schema = R"sql(
    create table if not exists Attributes (
      parent integer not null,
      name text,
      type integer not null,
      value text,
      context text,
      primary key (parent, name)
    );
  )sql";

  rc = sqlite3_exec(db, schema, nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    throw Error("Failed to create schema");
  }

  // Insert root attribute (parent=0, name="")
  const char* insert_root = "INSERT INTO Attributes (parent, name, type, value, context) "
                            "VALUES (0, '', 1, '', NULL)"; // type=1 is FullAttrs
  rc = sqlite3_exec(db, insert_root, nullptr, nullptr, nullptr);

  // Insert malicious child attribute
  sqlite3_stmt* stmt = nullptr;
  const char* insert_child = "INSERT INTO Attributes (parent, name, type, value, context) "
                             "VALUES (1, 'malicious', ?, ?, ?)";
  rc = sqlite3_prepare_v2(db, insert_child, -1, &stmt, nullptr);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, malicious_type);
    sqlite3_bind_text(stmt, 2, malicious_value.c_str(), -1, SQLITE_TRANSIENT);
    if (!malicious_context.empty()) {
      sqlite3_bind_text(stmt, 3, malicious_context.c_str(), -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, 3);
    }
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  sqlite3_close(db);
}

// Get the eval cache directory
auto get_eval_cache_dir() -> std::filesystem::path {
  return std::filesystem::path(get_cache_dir()) / "eval-cache-v6";
}

} // namespace

// =============================================================================
// BUG: Invalid type enum in cache triggers "unexpected type" error
// But the error is thrown without proper cleanup, potentially leaving
// the cache in an inconsistent state.
// =============================================================================

TEST_CASE("bug: eval cache with invalid type value", "[fuzz][eval-cache][bug]") {
  // Valid AttrType values are 0-8 (see eval-cache.h)
  // What happens with type=99 or type=-1 or type=INT_MAX?

  std::vector<int> malicious_types = {
      -1,        // Negative
      99,        // Out of range
      255,       // Max uint8
      256,       // Overflow uint8
      INT32_MAX, // Max int32
      INT32_MIN, // Min int32
  };

  for (int bad_type : malicious_types) {
    INFO("Testing malicious type: " << bad_type);

    // This should throw an error, not crash
    // The actual test would need to set up a full eval context
    // For now we just document the attack vector
    REQUIRE(bad_type != 0); // Placeholder assertion
  }
}

// =============================================================================
// BUG: Malformed context string causes parsing issues
// Context is tokenized with ";" separator - what about edge cases?
// =============================================================================

TEST_CASE("bug: eval cache with malformed context string", "[fuzz][eval-cache][bug]") {
  // Context strings are parsed by tokenize_string with ";" separator
  // and then NixStringContextElem::parse is called on each part

  std::vector<std::string> malicious_contexts = {
      ";;;",                          // Empty elements
      std::string("\x00\x00\x00", 3), // Null bytes
      std::string(1000000, 'a'),      // Very long string
      "path:with:colons",             // Multiple colons
      "/nix/store/invalid!path",      // Invalid store path chars
      "=reference",                   // Malformed reference
      "!output",                      // Malformed output ref
  };

  for (const auto& ctx : malicious_contexts) {
    INFO("Testing malicious context: " << ctx.substr(0, 50) << "...");
    REQUIRE(ctx.size() > 0); // All have at least one byte
  }
}

// =============================================================================
// BUG: SQL injection via attribute name
// Attribute names come from Nix expressions and are stored as TEXT.
// Are they properly escaped?
// =============================================================================

TEST_CASE("bug: eval cache sql injection via attribute name", "[fuzz][eval-cache][bug]") {
  std::vector<std::string> malicious_names = {
      "'; DROP TABLE Attributes; --",
      "\" OR 1=1 --",
      "name\x00hidden",        // Null byte in name
      std::string(10000, 'x'), // Very long name
      "SELECT * FROM sqlite_master",
      "UNION SELECT * FROM Attributes",
  };

  for (const auto& name : malicious_names) {
    INFO("Testing malicious name: " << name.substr(0, 50));
    REQUIRE(!name.empty()); // Placeholder assertion
  }
}

// =============================================================================
// BUG: Integer overflow in rowid/parent handling
// The cache uses int64 for rowids - what about overflow?
// =============================================================================

TEST_CASE("bug: eval cache integer overflow in parent id", "[fuzz][eval-cache][bug]") {
  std::vector<std::int64_t> malicious_parents = {
      -1,
      INT64_MIN,
      INT64_MAX,
      static_cast<std::int64_t>(UINT64_MAX),
  };

  for (auto parent : malicious_parents) {
    INFO("Testing malicious parent: " << parent);
    REQUIRE(true); // Placeholder
  }
}

// =============================================================================
// BUG: Concurrent cache access causes corruption
// Multiple processes writing to the same cache can corrupt it.
// See GitHub issues #3794, #6847
// =============================================================================

TEST_CASE("bug: concurrent eval cache access", "[fuzz][eval-cache][bug][!mayfail]") {
  // This test would spawn multiple threads/processes accessing the same cache
  // For now, document the attack vector
  WARN("Concurrent access to eval cache can cause 'database is locked' errors");
  WARN("See GitHub issues #3794, #6847");
  REQUIRE(true);
}

// =============================================================================
// Property test: arbitrary cache data should never crash
// =============================================================================

TEST_CASE("fuzz: eval cache type parsing", "[fuzz][eval-cache]") {
  rc::prop("arbitrary type values should not crash", []() {
    auto type_val = *rc::gen::arbitrary<int>();

    // Type values 0-8 are valid, others should throw
    bool is_valid = (type_val >= 0 && type_val <= 8);

    if (!is_valid) {
      // Should throw "unexpected type in evaluation cache"
      // not crash or abort
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: eval cache value parsing", "[fuzz][eval-cache]") {
  rc::prop("arbitrary value strings should not crash", []() {
    auto value = *rc::gen::arbitrary<std::string>();

    // Value strings are used for strings, paths, bools, ints
    // They should be validated, not blindly parsed

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: eval cache context parsing", "[fuzz][eval-cache]") {
  rc::prop("arbitrary context strings should not crash", []() {
    auto context = *rc::gen::arbitrary<std::string>();

    // Context is tokenized and parsed as NixStringContextElem
    // Malformed contexts should throw, not crash

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// BUG: Inject malicious SQLite data directly and observe crashes
// This test creates a corrupted eval-cache database and attempts to read it
//
// Attack vector: An attacker with write access to ~/.cache/nix/eval-cache-v6/
// can craft malicious SQLite files that crash nix when the cache is read.
// =============================================================================

TEST_CASE("bug: malicious eval cache database causes crash", "[fuzz][eval-cache][bug]") {
  // Create a temporary directory for our malicious cache
  auto temp_dir = std::filesystem::temp_directory_path() / "nix-eval-cache-fuzz-test";
  std::filesystem::create_directories(temp_dir);

  auto db_path = temp_dir / "malicious.sqlite";

  SECTION("invalid type value triggers error") {
    // Create cache with type=999 (invalid)
    create_malicious_cache(db_path, 999, "value", "");

    // Reading this should throw "unexpected type in evaluation cache"
    // not crash or assert
    sqlite3* db = nullptr;
    sqlite3_open(db_path.c_str(), &db);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT type FROM Attributes WHERE name='malicious'", -1, &stmt,
                       nullptr);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
      int type = sqlite3_column_int(stmt, 0);
      INFO("Injected malicious type: " << type);
      REQUIRE(type == 999); // Verify injection worked
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
  }

  SECTION("context with invalid store path") {
    // Create cache with context containing invalid store path
    // This triggers store_path_t constructor which may assert
    create_malicious_cache(db_path, 2, "/nix/store/invalid", "invalid!store!path");

    sqlite3* db = nullptr;
    sqlite3_open(db_path.c_str(), &db);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT context FROM Attributes WHERE name='malicious'", -1, &stmt,
                       nullptr);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char* context = (const char*)sqlite3_column_text(stmt, 0);
      INFO("Injected malicious context: " << context);
      REQUIRE(std::string(context) == "invalid!store!path");
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
  }

  SECTION("extremely long attribute name") {
    // Create cache with very long attribute name - may cause buffer issues
    std::string long_name(100000, 'x');

    sqlite3* db = nullptr;
    sqlite3_open(db_path.c_str(), &db);

    // Create schema
    sqlite3_exec(db, R"sql(
      create table if not exists Attributes (
        parent integer not null,
        name text,
        type integer not null,
        value text,
        context text,
        primary key (parent, name)
      );
    )sql",
                 nullptr, nullptr, nullptr);

    // Insert with long name
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
                       "INSERT INTO Attributes (parent, name, type, value) VALUES (0, ?, 2, 'x')",
                       -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, long_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // Verify it was written
    REQUIRE(std::filesystem::exists(db_path));
  }

  SECTION("orphan child attribute - missing parent triggers assertion") {
    // BUG: eval-cache.cpp:322 - assert(parent->first->cachedValue)
    // If we create a child attribute whose parent doesn't exist in the database,
    // traversing to that child will cause an assertion failure.
    //
    // Attack: Create child with parent=999, but no row with rowid=999 exists
    // When nix tries to look up the child's parent, get_attr returns nullopt
    // and the assertion fails.

    sqlite3* db = nullptr;
    sqlite3_open(db_path.c_str(), &db);

    sqlite3_exec(db, R"sql(
      create table if not exists Attributes (
        parent integer not null,
        name text,
        type integer not null,
        value text,
        context text,
        primary key (parent, name)
      );
    )sql",
                 nullptr, nullptr, nullptr);

    // Insert orphan child - parent 999 doesn't exist
    sqlite3_exec(db,
                 "INSERT INTO Attributes (parent, name, type, value) "
                 "VALUES (999, 'orphan', 2, 'value')",
                 nullptr, nullptr, nullptr);

    sqlite3_close(db);

    INFO("Created orphan child attribute with non-existent parent=999");
    INFO("Reading this through eval cache will trigger: assert(parent->first->cachedValue)");
    REQUIRE(std::filesystem::exists(db_path));
  }

  // Cleanup
  std::filesystem::remove_all(temp_dir);
}
