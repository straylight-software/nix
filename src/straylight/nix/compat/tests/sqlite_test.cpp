// straylight::nix::compat::sqlite tests
//
// Tests for SQLite RAII wrappers: Database, Statement, Transaction, Column

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compat/sqlite.h"
#include "straylight/nix/testing/temp_dir.h"

namespace sqlite = straylight::nix::compat;
namespace testing = straylight::nix::testing;

// ─────────────────────────────────────────────────────────────────────────────
// Database - Basic functionality
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Database opens in-memory database", "[sqlite][database]") {
  auto result = sqlite::Database::open_memory();
  REQUIRE(result.has_value());
  REQUIRE(result->is_open());
}

TEST_CASE("Database opens file database", "[sqlite][database]") {
  std::filesystem::path temp_path = testing::temp_directory_path() / "sqlite_test.db";

  // Clean up if exists
  std::filesystem::remove(temp_path);

  {
    auto result = sqlite::Database::open(temp_path, sqlite::OpenMode::read_write_create);
    REQUIRE(result.has_value());
    REQUIRE(result->is_open());
  }

  // File should exist
  REQUIRE(std::filesystem::exists(temp_path));

  // Clean up
  std::filesystem::remove(temp_path);
}

TEST_CASE("Database fails to open non-existent file in read-only mode", "[sqlite][database]") {
  std::filesystem::path temp_path = testing::temp_directory_path() / "sqlite_nonexistent.db";
  std::filesystem::remove(temp_path);

  auto result = sqlite::Database::open(temp_path, sqlite::OpenMode::read_only);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().error_code == SQLITE_CANTOPEN);
}

TEST_CASE("Database close is idempotent", "[sqlite][database]") {
  auto result = sqlite::Database::open_memory();
  REQUIRE(result.has_value());

  auto status1 = result->close();
  REQUIRE(status1.has_value());
  REQUIRE_FALSE(result->is_open());

  auto status2 = result->close();
  REQUIRE(status2.has_value());
}

TEST_CASE("Database move construction works", "[sqlite][database]") {
  auto result = sqlite::Database::open_memory();
  REQUIRE(result.has_value());

  sqlite3* original_handle = result->handle();

  sqlite::Database moved(std::move(*result));
  REQUIRE(moved.is_open());
  REQUIRE(moved.handle() == original_handle);
  REQUIRE_FALSE(result->is_open());
}

TEST_CASE("Database move assignment works", "[sqlite][database]") {
  auto result1 = sqlite::Database::open_memory();
  auto result2 = sqlite::Database::open_memory();
  REQUIRE(result1.has_value());
  REQUIRE(result2.has_value());

  sqlite3* handle2 = result2->handle();

  *result1 = std::move(*result2);
  REQUIRE(result1->is_open());
  REQUIRE(result1->handle() == handle2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Database - Execute
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Database executes simple SQL", "[sqlite][database][execute]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto status = database->execute("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)");
  REQUIRE(status.has_value());
}

TEST_CASE("Database execute returns error for invalid SQL", "[sqlite][database][execute]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto status = database->execute("INVALID SQL STATEMENT");
  REQUIRE_FALSE(status.has_value());
  REQUIRE(status.error().error_code == SQLITE_ERROR);
}

TEST_CASE("Database execute on closed database returns error", "[sqlite][database][execute]") {
  sqlite::Database database;
  REQUIRE_FALSE(database.is_open());

  auto status = database.execute("SELECT 1");
  REQUIRE_FALSE(status.has_value());
  REQUIRE(status.error().error_code == SQLITE_MISUSE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Database - Prepare
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Database prepares valid statement", "[sqlite][database][prepare]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT 1 + 1");
  REQUIRE(statement.has_value());
  REQUIRE(statement->valid());
}

TEST_CASE("Database prepare returns error for invalid SQL", "[sqlite][database][prepare]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("INVALID SQL");
  REQUIRE_FALSE(statement.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Statement - Basic functionality
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Statement execute works for INSERT", "[sqlite][statement][execute]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)");

  auto statement = database->prepare("INSERT INTO test (name) VALUES ('hello')");
  REQUIRE(statement.has_value());

  auto status = statement->execute();
  REQUIRE(status.has_value());

  REQUIRE(database->last_insert_rowid() == 1);
}

TEST_CASE("Statement execute_changes returns row count", "[sqlite][statement][execute]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (id INTEGER PRIMARY KEY, value INTEGER)");
  database->execute("INSERT INTO test (value) VALUES (1), (2), (3)");

  auto statement = database->prepare("UPDATE test SET value = value + 10");
  REQUIRE(statement.has_value());

  auto changes = statement->execute_changes();
  REQUIRE(changes.has_value());
  REQUIRE(*changes == 3);
}

TEST_CASE("Statement step and row access", "[sqlite][statement][step]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)");
  database->execute("INSERT INTO test (name) VALUES ('alice'), ('bob')");

  auto statement = database->prepare("SELECT id, name FROM test ORDER BY id");
  REQUIRE(statement.has_value());

  auto step1 = statement->step();
  REQUIRE(step1.has_value());
  REQUIRE(*step1 == true);

  auto row1 = statement->row();
  REQUIRE(row1.column_count() == 2);
  REQUIRE(row1[0].get<int>() == 1);
  REQUIRE(row1[1].get<std::string>() == "alice");

  auto step2 = statement->step();
  REQUIRE(step2.has_value());
  REQUIRE(*step2 == true);

  auto row2 = statement->row();
  REQUIRE(row2[0].get<int>() == 2);
  REQUIRE(row2[1].get<std::string>() == "bob");

  auto step3 = statement->step();
  REQUIRE(step3.has_value());
  REQUIRE(*step3 == false);
}

TEST_CASE("Statement move construction works", "[sqlite][statement]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT 1");
  REQUIRE(statement.has_value());

  sqlite::Statement moved(std::move(*statement));
  REQUIRE(moved.valid());
  REQUIRE_FALSE(statement->valid());
}

// ─────────────────────────────────────────────────────────────────────────────
// Statement - Parameter binding
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Statement bind integer parameter", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  REQUIRE(statement.has_value());

  statement->bind(1, 42);
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  auto result = query->step();
  REQUIRE(result.has_value());
  REQUIRE(*result == true);
  REQUIRE(query->row()[0].get<int>() == 42);
}

TEST_CASE("Statement bind int64 parameter", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  REQUIRE(statement.has_value());

  std::int64_t large_value = 9223372036854775807LL;
  statement->bind(1, large_value);
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].get<std::int64_t>() == large_value);
}

TEST_CASE("Statement bind double parameter", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value REAL)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind(1, 3.14159);
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].get<double>() == Catch::Approx(3.14159));
}

TEST_CASE("Statement bind string parameter", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value TEXT)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind(1, std::string_view("hello world"));
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].get<std::string>() == "hello world");
}

TEST_CASE("Statement bind C string parameter", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value TEXT)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind(1, "c string");
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].get<std::string>() == "c string");
}

TEST_CASE("Statement bind blob parameter", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value BLOB)");

  std::vector<std::uint8_t> blob = {0x00, 0x01, 0x02, 0xFF, 0xFE};

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind(1, blob);
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].get<std::vector<std::uint8_t>>() == blob);
}

TEST_CASE("Statement bind bool parameter", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind(1, true);
  statement->execute();

  statement->reset();
  statement->bind(1, false);
  statement->execute();

  auto query = database->prepare("SELECT value FROM test ORDER BY rowid");

  query->step();
  REQUIRE(query->row()[0].get<bool>() == true);

  query->step();
  REQUIRE(query->row()[0].get<bool>() == false);
}

TEST_CASE("Statement bind NULL parameter", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value TEXT)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind_null(1);
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].is_null());
}

TEST_CASE("Statement bind optional parameter", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value TEXT)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");

  // Bind with value
  std::optional<std::string> with_value = "present";
  statement->bind(1, with_value);
  statement->execute();

  // Bind without value
  statement->reset();
  std::optional<std::string> without_value;
  statement->bind(1, without_value);
  statement->execute();

  auto query = database->prepare("SELECT value FROM test ORDER BY rowid");

  query->step();
  REQUIRE_FALSE(query->row()[0].is_null());
  REQUIRE(query->row()[0].get<std::string>() == "present");

  query->step();
  REQUIRE(query->row()[0].is_null());
}

TEST_CASE("Statement bind by name", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (name TEXT, age INTEGER)");

  auto statement = database->prepare("INSERT INTO test VALUES (:name, :age)");
  REQUIRE(statement.has_value());

  statement->bind(":name", "alice");
  statement->bind(":age", 30);
  statement->execute();

  auto query = database->prepare("SELECT name, age FROM test");
  query->step();
  REQUIRE(query->row()[0].get<std::string>() == "alice");
  REQUIRE(query->row()[1].get<int>() == 30);
}

TEST_CASE("Statement bind unknown parameter name returns error", "[sqlite][statement][bind]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT ?1");
  auto status = statement->bind(":nonexistent", 42);

  REQUIRE_FALSE(status.has_value());
  REQUIRE(status.error().error_code == SQLITE_RANGE);
}

TEST_CASE("Statement reset allows reuse", "[sqlite][statement][reset]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");

  for (int index = 1; index <= 5; ++index) {
    statement->reset();
    statement->bind(1, index);
    statement->execute();
  }

  auto query = database->prepare("SELECT COUNT(*) FROM test");
  query->step();
  REQUIRE(query->row()[0].get<int>() == 5);
}

TEST_CASE("Statement parameter_count returns correct count", "[sqlite][statement]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT ?1, ?2, ?3");
  REQUIRE(statement.has_value());
  REQUIRE(statement->parameter_count() == 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Statement - Iteration
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Statement range-based iteration", "[sqlite][statement][iteration]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");
  database->execute("INSERT INTO test VALUES (10), (20), (30)");

  auto statement = database->prepare("SELECT value FROM test ORDER BY value");
  REQUIRE(statement.has_value());

  std::vector<int> values;
  for (auto row : *statement) {
    values.push_back(row[0].get<int>());
  }

  REQUIRE(values.size() == 3);
  REQUIRE(values[0] == 10);
  REQUIRE(values[1] == 20);
  REQUIRE(values[2] == 30);
}

TEST_CASE("Statement iteration over empty result", "[sqlite][statement][iteration]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");

  auto statement = database->prepare("SELECT value FROM test");
  REQUIRE(statement.has_value());

  int count = 0;
  for (auto row : *statement) {
    (void)row;
    ++count;
  }

  REQUIRE(count == 0);
}

TEST_CASE("Statement iteration can be restarted with reset", "[sqlite][statement][iteration]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");
  database->execute("INSERT INTO test VALUES (1), (2)");

  auto statement = database->prepare("SELECT value FROM test");
  REQUIRE(statement.has_value());

  // First iteration
  int sum1 = 0;
  for (auto row : *statement) {
    sum1 += row[0].get<int>();
  }
  REQUIRE(sum1 == 3);

  // Second iteration (begin() calls reset internally)
  int sum2 = 0;
  for (auto row : *statement) {
    sum2 += row[0].get<int>();
  }
  REQUIRE(sum2 == 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Column - Type-safe extraction
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Column get_or returns default for NULL", "[sqlite][column]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT NULL");
  statement->step();

  auto column = statement->row()[0];
  REQUIRE(column.is_null());
  REQUIRE(column.get_or<int>(42) == 42);
  REQUIRE(column.get_or<std::string>("default") == "default");
}

TEST_CASE("Column get_optional returns nullopt for NULL", "[sqlite][column]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT NULL");
  statement->step();

  auto column = statement->row()[0];
  REQUIRE_FALSE(column.get_optional<int>().has_value());
  REQUIRE_FALSE(column.get_optional<std::string>().has_value());
}

TEST_CASE("Column get_optional returns value for non-NULL", "[sqlite][column]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT 42");
  statement->step();

  auto column = statement->row()[0];
  auto optional = column.get_optional<int>();
  REQUIRE(optional.has_value());
  REQUIRE(*optional == 42);
}

TEST_CASE("Column name returns correct name", "[sqlite][column]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (my_column INTEGER)");
  database->execute("INSERT INTO test VALUES (1)");

  auto statement = database->prepare("SELECT my_column FROM test");
  statement->step();

  REQUIRE(statement->row().column_name(0) == "my_column");
}

TEST_CASE("Column type returns correct SQLite type", "[sqlite][column]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT 42, 3.14, 'text', X'00', NULL");
  statement->step();

  auto row = statement->row();
  REQUIRE(row[0].type() == SQLITE_INTEGER);
  REQUIRE(row[1].type() == SQLITE_FLOAT);
  REQUIRE(row[2].type() == SQLITE_TEXT);
  REQUIRE(row[3].type() == SQLITE_BLOB);
  REQUIRE(row[4].type() == SQLITE_NULL);
}

TEST_CASE("Column size_bytes returns correct size", "[sqlite][column]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT 'hello'");
  statement->step();

  REQUIRE(statement->row()[0].size_bytes() == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// Row - Column access
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Row column_count returns correct count", "[sqlite][row]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto statement = database->prepare("SELECT 1, 2, 3, 4, 5");
  statement->step();

  REQUIRE(statement->row().column_count() == 5);
}

TEST_CASE("Row find_column returns correct index", "[sqlite][row]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (alpha INTEGER, beta TEXT, gamma REAL)");
  database->execute("INSERT INTO test VALUES (1, 'two', 3.0)");

  auto statement = database->prepare("SELECT * FROM test");
  statement->step();

  auto row = statement->row();
  REQUIRE(row.find_column("alpha") == 0);
  REQUIRE(row.find_column("beta") == 1);
  REQUIRE(row.find_column("gamma") == 2);
  REQUIRE(row.find_column("nonexistent") == -1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Transaction - RAII behavior
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Transaction commits successfully", "[sqlite][transaction]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");

  {
    auto transaction = database->begin_transaction();
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->active());

    database->execute("INSERT INTO test VALUES (42)");

    auto status = transaction->commit();
    REQUIRE(status.has_value());
    REQUIRE_FALSE(transaction->active());
  }

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].get<int>() == 42);
}

TEST_CASE("Transaction rollback on scope exit", "[sqlite][transaction]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");

  {
    auto transaction = database->begin_transaction();
    REQUIRE(transaction.has_value());

    database->execute("INSERT INTO test VALUES (42)");

    // Transaction goes out of scope without commit - should rollback
  }

  auto query = database->prepare("SELECT COUNT(*) FROM test");
  query->step();
  REQUIRE(query->row()[0].get<int>() == 0);
}

TEST_CASE("Transaction explicit rollback", "[sqlite][transaction]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");
  database->execute("INSERT INTO test VALUES (1)");

  {
    auto transaction = database->begin_transaction();
    REQUIRE(transaction.has_value());

    database->execute("DELETE FROM test");

    auto status = transaction->rollback();
    REQUIRE(status.has_value());
  }

  auto query = database->prepare("SELECT COUNT(*) FROM test");
  query->step();
  REQUIRE(query->row()[0].get<int>() == 1);
}

TEST_CASE("Transaction commit on inactive transaction returns error", "[sqlite][transaction]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto transaction = database->begin_transaction();
  REQUIRE(transaction.has_value());

  transaction->commit();

  auto status = transaction->commit();
  REQUIRE_FALSE(status.has_value());
  REQUIRE(status.error().error_code == SQLITE_MISUSE);
}

TEST_CASE("Transaction move construction works", "[sqlite][transaction]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");

  auto transaction = database->begin_transaction();
  REQUIRE(transaction.has_value());

  sqlite::Transaction moved(std::move(*transaction));
  REQUIRE(moved.active());
  REQUIRE_FALSE(transaction->active());

  database->execute("INSERT INTO test VALUES (42)");
  moved.commit();

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].get<int>() == 42);
}

TEST_CASE("Transaction immediate type acquires write lock", "[sqlite][transaction]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto transaction = database->begin_transaction(sqlite::TransactionType::immediate);
  REQUIRE(transaction.has_value());

  transaction->commit();
}

TEST_CASE("Transaction exclusive type acquires exclusive lock", "[sqlite][transaction]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto transaction = database->begin_transaction(sqlite::TransactionType::exclusive);
  REQUIRE(transaction.has_value());

  transaction->commit();
}

// ─────────────────────────────────────────────────────────────────────────────
// Database - Configuration
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Database configure_as_cache sets pragmas", "[sqlite][database][config]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto status = database->configure_as_cache();
  REQUIRE(status.has_value());
}

TEST_CASE("Database enable_foreign_keys works", "[sqlite][database][config]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto status = database->enable_foreign_keys();
  REQUIRE(status.has_value());

  // Verify it's enabled
  auto statement = database->prepare("PRAGMA foreign_keys");
  statement->step();
  REQUIRE(statement->row()[0].get<int>() == 1);
}

TEST_CASE("Database set_busy_timeout works", "[sqlite][database][config]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  auto status = database->set_busy_timeout(5000);
  REQUIRE(status.has_value());
}

TEST_CASE("Database enable_wal works for file database", "[sqlite][database][config]") {
  std::filesystem::path temp_path = testing::temp_directory_path() / "sqlite_wal_test.db";
  std::filesystem::remove(temp_path);

  {
    auto database = sqlite::Database::open(temp_path);
    REQUIRE(database.has_value());

    auto status = database->enable_wal();
    REQUIRE(status.has_value());
  }

  // Clean up
  std::filesystem::remove(temp_path);
  std::filesystem::remove(temp_path.string() + "-wal");
  std::filesystem::remove(temp_path.string() + "-shm");
}

// ─────────────────────────────────────────────────────────────────────────────
// Database - Metadata
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Database last_insert_rowid returns correct value", "[sqlite][database][metadata]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)");
  database->execute("INSERT INTO test (value) VALUES ('first')");

  REQUIRE(database->last_insert_rowid() == 1);

  database->execute("INSERT INTO test (value) VALUES ('second')");

  REQUIRE(database->last_insert_rowid() == 2);
}

TEST_CASE("Database changes returns correct value", "[sqlite][database][metadata]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");
  database->execute("INSERT INTO test VALUES (1), (2), (3)");

  database->execute("UPDATE test SET value = value + 10 WHERE value < 3");

  REQUIRE(database->changes() == 2);
}

TEST_CASE("Database total_changes accumulates", "[sqlite][database][metadata]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value INTEGER)");

  int initial_changes = database->total_changes();

  database->execute("INSERT INTO test VALUES (1)");
  database->execute("INSERT INTO test VALUES (2)");
  database->execute("DELETE FROM test WHERE value = 1");

  REQUIRE(database->total_changes() == initial_changes + 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// SqliteError - Error information
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SqliteError is_busy returns true for SQLITE_BUSY", "[sqlite][error]") {
  sqlite::SqliteError error{
      .error_code = SQLITE_BUSY,
      .extended_error_code = SQLITE_BUSY,
      .message = "database is locked",
      .sql = "SELECT 1",
  };

  REQUIRE(error.is_busy());
}

TEST_CASE("SqliteError is_busy returns true for SQLITE_LOCKED", "[sqlite][error]") {
  sqlite::SqliteError error{
      .error_code = SQLITE_LOCKED,
      .extended_error_code = SQLITE_LOCKED,
      .message = "database is locked",
      .sql = "SELECT 1",
  };

  REQUIRE(error.is_busy());
}

TEST_CASE("SqliteError is_constraint_violation works", "[sqlite][error]") {
  sqlite::SqliteError error{
      .error_code = SQLITE_CONSTRAINT,
      .extended_error_code = SQLITE_CONSTRAINT_UNIQUE,
      .message = "UNIQUE constraint failed",
      .sql = "INSERT ...",
  };

  REQUIRE(error.is_constraint_violation());
}

TEST_CASE("SqliteError error_name returns readable string", "[sqlite][error]") {
  sqlite::SqliteError error{
      .error_code = SQLITE_NOTFOUND,
      .extended_error_code = SQLITE_NOTFOUND,
      .message = "",
      .sql = "",
  };

  auto name = error.error_name();
  REQUIRE_FALSE(name.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases and stress tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Large blob handling", "[sqlite][stress]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (data BLOB)");

  // Create a 1MB blob
  std::vector<std::uint8_t> large_blob(1024 * 1024);
  for (std::size_t index = 0; index < large_blob.size(); ++index) {
    large_blob[index] = static_cast<std::uint8_t>(index % 256);
  }

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind(1, large_blob);
  statement->execute();

  auto query = database->prepare("SELECT data FROM test");
  query->step();

  auto retrieved = query->row()[0].get<std::vector<std::uint8_t>>();
  REQUIRE(retrieved.size() == large_blob.size());
  REQUIRE(retrieved == large_blob);
}

TEST_CASE("Many rows handling", "[sqlite][stress]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)");

  auto transaction = database->begin_transaction();

  auto statement = database->prepare("INSERT INTO test (value) VALUES (?1)");
  for (int index = 0; index < 10000; ++index) {
    statement->reset();
    statement->bind(1, "row" + std::to_string(index));
    statement->execute();
  }

  transaction->commit();

  auto query = database->prepare("SELECT COUNT(*) FROM test");
  query->step();
  REQUIRE(query->row()[0].get<int>() == 10000);
}

TEST_CASE("Empty string handling", "[sqlite][edge]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value TEXT)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind(1, std::string_view(""));
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  query->step();

  auto column = query->row()[0];
  REQUIRE_FALSE(column.is_null());
  REQUIRE(column.get<std::string>().empty());
}

TEST_CASE("Empty blob handling", "[sqlite][edge]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (data BLOB)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind(1, std::vector<std::uint8_t>{});
  statement->execute();

  auto query = database->prepare("SELECT data FROM test");
  query->step();

  auto column = query->row()[0];
  auto blob = column.get<std::vector<std::uint8_t>>();
  REQUIRE(blob.empty());
}

TEST_CASE("Unicode string handling", "[sqlite][edge]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value TEXT)");

  std::string unicode_string =
      "Hello \xC3\xA9\xC3\xA0\xC3\xBC \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80";

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  statement->bind(1, std::string_view(unicode_string));
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].get<std::string>() == unicode_string);
}

TEST_CASE("NULL C string binding", "[sqlite][edge]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  database->execute("CREATE TABLE test (value TEXT)");

  auto statement = database->prepare("INSERT INTO test VALUES (?1)");
  const char* null_string = nullptr;
  statement->bind(1, null_string);
  statement->execute();

  auto query = database->prepare("SELECT value FROM test");
  query->step();
  REQUIRE(query->row()[0].is_null());
}

TEST_CASE("Multiple databases simultaneously", "[sqlite][edge]") {
  auto database1 = sqlite::Database::open_memory();
  auto database2 = sqlite::Database::open_memory();

  REQUIRE(database1.has_value());
  REQUIRE(database2.has_value());

  database1->execute("CREATE TABLE test1 (value INTEGER)");
  database2->execute("CREATE TABLE test2 (value TEXT)");

  database1->execute("INSERT INTO test1 VALUES (42)");
  database2->execute("INSERT INTO test2 VALUES ('hello')");

  auto query1 = database1->prepare("SELECT value FROM test1");
  auto query2 = database2->prepare("SELECT value FROM test2");

  query1->step();
  query2->step();

  REQUIRE(query1->row()[0].get<int>() == 42);
  REQUIRE(query2->row()[0].get<std::string>() == "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// retry_on_busy helper
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("retry_on_busy succeeds on first try", "[sqlite][retry]") {
  auto database = sqlite::Database::open_memory();
  REQUIRE(database.has_value());

  int call_count = 0;
  auto result = sqlite::retry_on_busy([&]() -> sqlite::SqliteResult<int> {
    ++call_count;
    return 42;
  });

  REQUIRE(result.has_value());
  REQUIRE(*result == 42);
  REQUIRE(call_count == 1);
}

TEST_CASE("retry_on_busy returns non-busy errors immediately", "[sqlite][retry]") {
  int call_count = 0;
  auto result = sqlite::retry_on_busy([&]() -> sqlite::SqliteResult<int> {
    ++call_count;
    return std::unexpected(sqlite::SqliteError{
        .error_code = SQLITE_ERROR,
        .extended_error_code = SQLITE_ERROR,
        .message = "syntax error",
        .sql = "INVALID",
    });
  });

  REQUIRE_FALSE(result.has_value());
  REQUIRE(call_count == 1);
}
