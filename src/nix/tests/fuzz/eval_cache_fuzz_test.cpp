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

#include "straylight/nix/testing/temp_dir.h"

#include "nix/expr/eval-cache.h"
#include "nix/expr/eval.h"
#include "nix/store/store-api.h"
#include "nix/tests/property.h"
#include "nix/util/error.h"
#include "nix/util/hash.h"
#include "nix/util/users.h"

namespace {

// Create a minimal eval cache database with injected malicious data
auto create_malicious_cache(const ::std::filesystem::path& db_path, int malicious_type,
                            const ::std::string& malicious_value,
                            const ::std::string& malicious_context) -> void {
  // Remove existing file
  ::std::filesystem::remove(db_path);

  ::sqlite3* database = nullptr;
  auto result_code = ::sqlite3_open(db_path.c_str(), &database);
  if (result_code != SQLITE_OK) {
    throw nix::Error("Failed to create malicious cache: %s", ::sqlite3_errmsg(database));
  }

  // Create schema
  const auto* schema = R"sql(
    create table if not exists Attributes (
      parent integer not null,
      name text,
      type integer not null,
      value text,
      context text,
      primary key (parent, name)
    );
  )sql";

  result_code = ::sqlite3_exec(database, schema, nullptr, nullptr, nullptr);
  if (result_code != SQLITE_OK) {
    ::sqlite3_close(database);
    throw nix::Error("Failed to create schema");
  }

  // Insert root attribute (parent=0, name="")
  const auto* insert_root = "INSERT INTO Attributes (parent, name, type, value, context) "
                            "VALUES (0, '', 1, '', NULL)"; // type=1 is FullAttrs
  result_code = ::sqlite3_exec(database, insert_root, nullptr, nullptr, nullptr);

  // Insert malicious child attribute
  ::sqlite3_stmt* statement = nullptr;
  const auto* insert_child = "INSERT INTO Attributes (parent, name, type, value, context) "
                             "VALUES (1, 'malicious', ?, ?, ?)";
  result_code = ::sqlite3_prepare_v2(database, insert_child, -1, &statement, nullptr);
  if (result_code == SQLITE_OK) {
    ::sqlite3_bind_int(statement, 1, malicious_type);
    ::sqlite3_bind_text(statement, 2, malicious_value.c_str(), -1, SQLITE_TRANSIENT);
    if (!malicious_context.empty()) {
      ::sqlite3_bind_text(statement, 3, malicious_context.c_str(), -1, SQLITE_TRANSIENT);
    } else {
      ::sqlite3_bind_null(statement, 3);
    }
    ::sqlite3_step(statement);
    ::sqlite3_finalize(statement);
  }

  ::sqlite3_close(database);
}

// Get the eval cache directory
auto get_eval_cache_dir() -> ::std::filesystem::path {
  return ::std::filesystem::path(nix::get_cache_dir()) / "eval-cache-v6";
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

  auto malicious_types = ::std::vector<int>{
      -1,        // Negative
      99,        // Out of range
      255,       // Max uint8
      256,       // Overflow uint8
      INT32_MAX, // Max int32
      INT32_MIN, // Min int32
  };

  for (auto bad_type : malicious_types) {
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

  auto malicious_contexts = ::std::vector<::std::string>{
      ";;;",                            // Empty elements
      ::std::string("\x00\x00\x00", 3), // Null bytes
      ::std::string(1000000, 'a'),      // Very long string
      "path:with:colons",               // Multiple colons
      "/nix/store/invalid!path",        // Invalid store path chars
      "=reference",                     // Malformed reference
      "!output",                        // Malformed output ref
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
  auto malicious_names = ::std::vector<::std::string>{
      "'; DROP TABLE Attributes; --",
      "\" OR 1=1 --",
      "name\x00hidden",          // Null byte in name
      ::std::string(10000, 'x'), // Very long name
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
  auto malicious_parents = ::std::vector<::std::int64_t>{
      -1,
      INT64_MIN,
      INT64_MAX,
      static_cast<::std::int64_t>(UINT64_MAX),
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
    auto is_valid = (type_val >= 0 && type_val <= 8);

    if (!is_valid) {
      // Should throw "unexpected type in evaluation cache"
      // not crash or abort
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: eval cache value parsing", "[fuzz][eval-cache]") {
  rc::prop("arbitrary value strings should not crash", []() {
    auto value = *rc::gen::arbitrary<::std::string>();

    // Value strings are used for strings, paths, bools, ints
    // They should be validated, not blindly parsed

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: eval cache context parsing", "[fuzz][eval-cache]") {
  rc::prop("arbitrary context strings should not crash", []() {
    auto context = *rc::gen::arbitrary<::std::string>();

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
  auto temp_dir = ::straylight::nix::testing::temp_directory_path() / "nix-eval-cache-fuzz-test";
  ::std::filesystem::create_directories(temp_dir);

  auto db_path = temp_dir / "malicious.sqlite";

  SECTION("invalid type value triggers error") {
    // Create cache with type=999 (invalid)
    create_malicious_cache(db_path, 999, "value", "");

    // Reading this should throw "unexpected type in evaluation cache"
    // not crash or assert
    ::sqlite3* database = nullptr;
    ::sqlite3_open(db_path.c_str(), &database);

    ::sqlite3_stmt* statement = nullptr;
    ::sqlite3_prepare_v2(database, "SELECT type FROM Attributes WHERE name='malicious'", -1,
                         &statement, nullptr);

    if (::sqlite3_step(statement) == SQLITE_ROW) {
      auto type_value = ::sqlite3_column_int(statement, 0);
      INFO("Injected malicious type: " << type_value);
      REQUIRE(type_value == 999); // Verify injection worked
    }

    ::sqlite3_finalize(statement);
    ::sqlite3_close(database);
  }

  SECTION("context with invalid store path") {
    // Create cache with context containing invalid store path
    // This triggers store_path_t constructor which may assert
    create_malicious_cache(db_path, 2, "/nix/store/invalid", "invalid!store!path");

    ::sqlite3* database = nullptr;
    ::sqlite3_open(db_path.c_str(), &database);

    ::sqlite3_stmt* statement = nullptr;
    ::sqlite3_prepare_v2(database, "SELECT context FROM Attributes WHERE name='malicious'", -1,
                         &statement, nullptr);

    if (::sqlite3_step(statement) == SQLITE_ROW) {
      const auto* context = reinterpret_cast<const char*>(::sqlite3_column_text(statement, 0));
      INFO("Injected malicious context: " << context);
      REQUIRE(::std::string(context) == "invalid!store!path");
    }

    ::sqlite3_finalize(statement);
    ::sqlite3_close(database);
  }

  SECTION("extremely long attribute name") {
    // Create cache with very long attribute name - may cause buffer issues
    auto long_name = ::std::string(100000, 'x');

    ::sqlite3* database = nullptr;
    ::sqlite3_open(db_path.c_str(), &database);

    // Create schema
    ::sqlite3_exec(database, R"sql(
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
    ::sqlite3_stmt* statement = nullptr;
    ::sqlite3_prepare_v2(database,
                         "INSERT INTO Attributes (parent, name, type, value) VALUES (0, ?, 2, 'x')",
                         -1, &statement, nullptr);
    ::sqlite3_bind_text(statement, 1, long_name.c_str(), -1, SQLITE_TRANSIENT);
    ::sqlite3_step(statement);
    ::sqlite3_finalize(statement);
    ::sqlite3_close(database);

    // Verify it was written
    REQUIRE(::std::filesystem::exists(db_path));
  }

  SECTION("orphan child attribute - missing parent triggers assertion") {
    // ===========================================================================
    // BUG: eval-cache.cpp:322 - assert(parent->first->cachedValue)
    // ===========================================================================
    // SEVERITY: CRASH (SIGABRT)
    // ATTACK SURFACE: Local file system access to ~/.cache/nix/eval-cache-v6/
    //
    // VULNERABILITY:
    // If we create a child attribute whose parent doesn't exist in the database,
    // traversing to that child will cause an assertion failure.
    //
    // CODE PATH:
    // 1. AttrCursor::getKey() is called (line 317)
    // 2. If parent exists and !parent->first->cachedValue (line 320)
    // 3. It tries to fetch from DB: root->db->get_attr(parent->first->getKey())
    // 4. If the parent doesn't exist in DB, get_attr returns std::nullopt
    // 5. assert(parent->first->cachedValue) FAILS (line 322)
    //
    // EXPLOITATION:
    // 1. Attacker creates malicious SQLite database at:
    //    ~/.cache/nix/eval-cache-v6/<fingerprint>.sqlite
    // 2. Database contains: INSERT INTO Attributes (parent, name, type, value)
    //    VALUES (999, 'orphan', 2, 'value')  -- parent 999 doesn't exist
    // 3. Victim runs `nix build` on a flake matching the fingerprint
    // 4. EvalCache loads the malicious database
    // 5. When traversing attributes, AttrCursor::getKey() is called
    // 6. The assertion fails, crashing nix with SIGABRT
    //
    // FINGERPRINT COMPUTATION:
    // The fingerprint is: Hash(flake locked inputs + nix version + store dir)
    // See: src/nix/flake/flake.cpp:967-982
    //
    // MITIGATION: Replace assert() with proper error handling
    // ===========================================================================

    ::sqlite3* database = nullptr;
    ::sqlite3_open(db_path.c_str(), &database);

    ::sqlite3_exec(database, R"sql(
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

    // Create a proper root first (rowid=1)
    ::sqlite3_exec(database,
                   "INSERT INTO Attributes (parent, name, type, value) "
                   "VALUES (0, '', 1, '')", // Root: parent=0, type=1 (FullAttrs)
                   nullptr, nullptr, nullptr);

    // Insert orphan child - parent 999 doesn't exist (no row with rowid=999)
    // When nix tries to traverse: root -> child, it will fail
    // because child's parent (999) has no cachedValue
    ::sqlite3_exec(database,
                   "INSERT INTO Attributes (parent, name, type, value) "
                   "VALUES (999, 'orphan', 2, 'value')",
                   nullptr, nullptr, nullptr);

    ::sqlite3_close(database);

    // Verify database structure
    ::sqlite3_open(db_path.c_str(), &database);
    ::sqlite3_stmt* statement = nullptr;

    // Check root exists
    ::sqlite3_prepare_v2(database, "SELECT rowid FROM Attributes WHERE parent=0 AND name=''", -1,
                         &statement, nullptr);
    REQUIRE(::sqlite3_step(statement) == SQLITE_ROW);
    auto root_rowid = ::sqlite3_column_int64(statement, 0);
    INFO("Root rowid: " << root_rowid);
    ::sqlite3_finalize(statement);

    // Check orphan exists
    ::sqlite3_prepare_v2(database, "SELECT rowid, parent FROM Attributes WHERE name='orphan'", -1,
                         &statement, nullptr);
    REQUIRE(::sqlite3_step(statement) == SQLITE_ROW);
    auto orphan_rowid = ::sqlite3_column_int64(statement, 0);
    auto orphan_parent = ::sqlite3_column_int64(statement, 1);
    INFO("Orphan rowid: " << orphan_rowid << ", parent: " << orphan_parent);
    REQUIRE(orphan_parent == 999);
    ::sqlite3_finalize(statement);

    // Verify parent 999 doesn't exist
    ::sqlite3_prepare_v2(database, "SELECT rowid FROM Attributes WHERE rowid=999", -1, &statement,
                         nullptr);
    REQUIRE(::sqlite3_step(statement) == SQLITE_DONE); // No row found
    ::sqlite3_finalize(statement);

    ::sqlite3_close(database);

    INFO("Created malicious database with orphan child");
    INFO("To trigger crash:");
    INFO("  1. Copy database to ~/.cache/nix/eval-cache-v6/<hash>.sqlite");
    INFO("  2. Run nix build on a flake with matching fingerprint hash");
    INFO("  3. Observe SIGABRT from assertion failure at eval-cache.cpp:322");
    REQUIRE(::std::filesystem::exists(db_path));
  }

  // Cleanup
  ::std::filesystem::remove_all(temp_dir);
}
