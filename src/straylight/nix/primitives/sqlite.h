// straylight::nix::primitives::sqlite - SQLite RAII wrappers
//
// Modern C++23 SQLite wrappers with exception-free error handling.
// Provides SQLiteCpp-style RAII classes:
//   - Database     - RAII handle for sqlite3*
//   - Statement    - RAII prepared statement with parameter binding
//   - Transaction  - RAII transaction with commit/rollback
//   - Column       - Type-safe result column access
//
// All operations return std::expected for error handling.
// Supports iterating over result sets with range-based for loops.

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <sqlite3.h>

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// Error types
// ─────────────────────────────────────────────────────────────────────────────

/// SQLite error information
struct SqliteError {
  int error_code = 0;
  int extended_error_code = 0;
  std::string message;
  std::string sql;

  /// Check if this is a SQLITE_BUSY error
  [[nodiscard]] bool is_busy() const noexcept {
    return error_code == SQLITE_BUSY || error_code == SQLITE_LOCKED;
  }

  /// Check if this is a constraint violation
  [[nodiscard]] bool is_constraint_violation() const noexcept {
    return error_code == SQLITE_CONSTRAINT;
  }

  /// Get human-readable error name
  [[nodiscard]] std::string error_name() const { return sqlite3_errstr(error_code); }
};

/// Result type for SQLite operations
template <typename T>
using SqliteResult = std::expected<T, SqliteError>;

/// Result type for void operations
using SqliteStatus = std::expected<void, SqliteError>;

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

class Database;
class Statement;
class Transaction;
class Column;
class Row;
class RowIterator;

// ─────────────────────────────────────────────────────────────────────────────
// OpenMode - Database open mode flags
// ─────────────────────────────────────────────────────────────────────────────

/// Database open mode
enum class OpenMode {
  /// Open in read-write mode, create if not exists
  read_write_create,
  /// Open in read-write mode, fail if not exists
  read_write,
  /// Open in read-only mode
  read_only,
  /// Open in immutable mode (no journal/wal files created)
  immutable,
  /// Open in-memory database
  memory,
};

// ─────────────────────────────────────────────────────────────────────────────
// Column - Type-safe result column access
// ─────────────────────────────────────────────────────────────────────────────

/// Type-safe accessor for a result column.
///
/// Provides get<T>() for type-safe extraction with automatic
/// type conversion where safe.
class Column {
public:
  Column(sqlite3_stmt* statement, int index) noexcept : statement_(statement), index_(index) {}

  /// Get column index
  [[nodiscard]] int index() const noexcept { return index_; }

  /// Get column name
  [[nodiscard]] std::string_view name() const noexcept {
    const char* name = sqlite3_column_name(statement_, index_);
    return name ? std::string_view(name) : std::string_view();
  }

  /// Check if column is NULL
  [[nodiscard]] bool is_null() const noexcept {
    return sqlite3_column_type(statement_, index_) == SQLITE_NULL;
  }

  /// Get column type (SQLITE_INTEGER, SQLITE_FLOAT, SQLITE_TEXT, SQLITE_BLOB, SQLITE_NULL)
  [[nodiscard]] int type() const noexcept { return sqlite3_column_type(statement_, index_); }

  /// Get column byte size
  [[nodiscard]] int size_bytes() const noexcept { return sqlite3_column_bytes(statement_, index_); }

  // ─────────────────────────────────────────────────────────────────────────
  // Type-safe extraction
  // ─────────────────────────────────────────────────────────────────────────

  /// Get value as specific type.
  ///
  /// Supported types:
  ///   - int, std::int64_t, std::uint64_t (integers)
  ///   - double (floating point)
  ///   - std::string, std::string_view (text)
  ///   - std::vector<std::uint8_t>, std::span<const std::uint8_t> (blob)
  ///   - bool (integer != 0)
  template <typename T>
  [[nodiscard]] T get() const;

  /// Get value with default if NULL
  template <typename T>
  [[nodiscard]] T get_or(T default_value) const {
    if (is_null()) {
      return default_value;
    }
    return get<T>();
  }

  /// Get value as optional (std::nullopt if NULL)
  template <typename T>
  [[nodiscard]] std::optional<T> get_optional() const {
    if (is_null()) {
      return std::nullopt;
    }
    return get<T>();
  }

private:
  sqlite3_stmt* statement_;
  int index_;
};

// Template specializations for Column::get<T>()
template <>
inline std::int64_t Column::get<std::int64_t>() const {
  return sqlite3_column_int64(statement_, index_);
}

template <>
inline int Column::get<int>() const {
  return sqlite3_column_int(statement_, index_);
}

template <>
inline std::uint64_t Column::get<std::uint64_t>() const {
  return static_cast<std::uint64_t>(sqlite3_column_int64(statement_, index_));
}

template <>
inline double Column::get<double>() const {
  return sqlite3_column_double(statement_, index_);
}

template <>
inline bool Column::get<bool>() const {
  return sqlite3_column_int(statement_, index_) != 0;
}

template <>
inline std::string Column::get<std::string>() const {
  const unsigned char* text = sqlite3_column_text(statement_, index_);
  int length = sqlite3_column_bytes(statement_, index_);
  if (text == nullptr || length == 0) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(length));
}

template <>
inline std::string_view Column::get<std::string_view>() const {
  const unsigned char* text = sqlite3_column_text(statement_, index_);
  int length = sqlite3_column_bytes(statement_, index_);
  if (text == nullptr || length == 0) {
    return std::string_view();
  }
  return std::string_view(reinterpret_cast<const char*>(text), static_cast<std::size_t>(length));
}

template <>
inline std::vector<std::uint8_t> Column::get<std::vector<std::uint8_t>>() const {
  const void* blob = sqlite3_column_blob(statement_, index_);
  int length = sqlite3_column_bytes(statement_, index_);
  if (blob == nullptr || length == 0) {
    return std::vector<std::uint8_t>();
  }
  const auto* bytes = static_cast<const std::uint8_t*>(blob);
  return std::vector<std::uint8_t>(bytes, bytes + length);
}

// ─────────────────────────────────────────────────────────────────────────────
// Row - Access to all columns in a result row
// ─────────────────────────────────────────────────────────────────────────────

/// Access to all columns in a result row.
///
/// Provides both index-based and name-based column access.
class Row {
public:
  explicit Row(sqlite3_stmt* statement) noexcept : statement_(statement) {}

  /// Get number of columns
  [[nodiscard]] int column_count() const noexcept { return sqlite3_column_count(statement_); }

  /// Get column by index
  [[nodiscard]] Column operator[](int index) const noexcept { return Column(statement_, index); }

  /// Get column by index (alias)
  [[nodiscard]] Column column(int index) const noexcept { return Column(statement_, index); }

  /// Get column name by index
  [[nodiscard]] std::string_view column_name(int index) const noexcept {
    const char* name = sqlite3_column_name(statement_, index);
    return name ? std::string_view(name) : std::string_view();
  }

  /// Find column index by name (returns -1 if not found)
  [[nodiscard]] int find_column(std::string_view name) const noexcept {
    int count = column_count();
    for (int index = 0; index < count; ++index) {
      if (column_name(index) == name) {
        return index;
      }
    }
    return -1;
  }

  /// Get underlying statement handle
  [[nodiscard]] sqlite3_stmt* handle() const noexcept { return statement_; }

private:
  sqlite3_stmt* statement_;
};

// ─────────────────────────────────────────────────────────────────────────────
// RowIterator - Iterator for result sets
// ─────────────────────────────────────────────────────────────────────────────

/// Iterator for result rows.
///
/// Supports range-based for loops over Statement results.
class RowIterator {
public:
  using iterator_category = std::input_iterator_tag;
  using value_type = Row;
  using difference_type = std::ptrdiff_t;
  using pointer = const Row*;
  using reference = const Row&;

  /// End iterator
  RowIterator() noexcept : statement_(nullptr), has_row_(false) {}

  /// Iterator with current row
  explicit RowIterator(sqlite3_stmt* statement, bool has_row) noexcept
      : statement_(statement), has_row_(has_row) {}

  [[nodiscard]] Row operator*() const noexcept { return Row(statement_); }

  RowIterator& operator++() {
    if (statement_ != nullptr) {
      int result = sqlite3_step(statement_);
      has_row_ = (result == SQLITE_ROW);
    }
    return *this;
  }

  RowIterator operator++(int) {
    RowIterator tmp = *this;
    ++(*this);
    return tmp;
  }

  [[nodiscard]] bool operator==(const RowIterator& other) const noexcept {
    // End iterator comparison: both have no row
    if (!has_row_ && !other.has_row_) {
      return true;
    }
    // Same statement and state
    return statement_ == other.statement_ && has_row_ == other.has_row_;
  }

  [[nodiscard]] bool operator!=(const RowIterator& other) const noexcept {
    return !(*this == other);
  }

private:
  sqlite3_stmt* statement_;
  bool has_row_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Statement - RAII prepared statement
// ─────────────────────────────────────────────────────────────────────────────

/// RAII prepared statement with parameter binding and result iteration.
///
/// Usage:
///   auto statement = database.prepare("SELECT * FROM users WHERE id = ?1");
///   if (statement) {
///     statement->bind(1, user_id);
///     for (auto row : *statement) {
///       auto name = row[0].get<std::string>();
///     }
///   }
class Statement {
public:
  /// Construct empty statement
  Statement() noexcept = default;

  /// Construct from existing handle (takes ownership)
  Statement(sqlite3* database, sqlite3_stmt* statement, std::string sql) noexcept
      : database_(database), statement_(statement), sql_(std::move(sql)) {}

  // Non-copyable
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  // Movable
  Statement(Statement&& other) noexcept
      : database_(std::exchange(other.database_, nullptr)),
        statement_(std::exchange(other.statement_, nullptr)),
        sql_(std::move(other.sql_)) {}

  Statement& operator=(Statement&& other) noexcept {
    if (this != &other) {
      finalize();
      database_ = std::exchange(other.database_, nullptr);
      statement_ = std::exchange(other.statement_, nullptr);
      sql_ = std::move(other.sql_);
    }
    return *this;
  }

  ~Statement() noexcept { finalize(); }

  /// Check if statement is valid
  [[nodiscard]] bool valid() const noexcept { return statement_ != nullptr; }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

  /// Get SQL string
  [[nodiscard]] const std::string& sql() const noexcept { return sql_; }

  /// Get underlying handle
  [[nodiscard]] sqlite3_stmt* handle() const noexcept { return statement_; }

  // ─────────────────────────────────────────────────────────────────────────
  // Parameter binding (?1, ?2, etc. or :name, @name, $name)
  // ─────────────────────────────────────────────────────────────────────────

  /// Bind NULL to parameter
  SqliteStatus bind_null(int index) {
    int result = sqlite3_bind_null(statement_, index);
    return check_bind_result(result);
  }

  /// Bind integer value
  SqliteStatus bind(int index, std::int64_t value) {
    int result = sqlite3_bind_int64(statement_, index, value);
    return check_bind_result(result);
  }

  /// Bind integer value (int overload)
  SqliteStatus bind(int index, int value) {
    int result = sqlite3_bind_int(statement_, index, value);
    return check_bind_result(result);
  }

  /// Bind double value
  SqliteStatus bind(int index, double value) {
    int result = sqlite3_bind_double(statement_, index, value);
    return check_bind_result(result);
  }

  /// Bind string value (copies the string)
  SqliteStatus bind(int index, std::string_view value) {
    int result = sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                                   SQLITE_TRANSIENT);
    return check_bind_result(result);
  }

  /// Bind string value (C string, copies)
  SqliteStatus bind(int index, const char* value) {
    if (value == nullptr) {
      return bind_null(index);
    }
    int result = sqlite3_bind_text(statement_, index, value, -1, SQLITE_TRANSIENT);
    return check_bind_result(result);
  }

  /// Bind blob value (copies the data)
  SqliteStatus bind(int index, std::span<const std::uint8_t> value) {
    int result = sqlite3_bind_blob(statement_, index, value.data(), static_cast<int>(value.size()),
                                   SQLITE_TRANSIENT);
    return check_bind_result(result);
  }

  /// Bind blob value from vector
  SqliteStatus bind(int index, const std::vector<std::uint8_t>& value) {
    return bind(index, std::span<const std::uint8_t>(value));
  }

  /// Bind bool value (as integer 0/1)
  SqliteStatus bind(int index, bool value) { return bind(index, value ? 1 : 0); }

  /// Bind optional value (NULL if empty)
  template <typename T>
  SqliteStatus bind(int index, const std::optional<T>& value) {
    if (value.has_value()) {
      return bind(index, *value);
    }
    return bind_null(index);
  }

  /// Get parameter count
  [[nodiscard]] int parameter_count() const noexcept {
    return sqlite3_bind_parameter_count(statement_);
  }

  /// Get parameter index by name (returns 0 if not found)
  [[nodiscard]] int parameter_index(const char* name) const noexcept {
    return sqlite3_bind_parameter_index(statement_, name);
  }

  /// Bind by parameter name
  template <typename T>
  SqliteStatus bind(const char* name, T&& value) {
    int index = parameter_index(name);
    if (index == 0) {
      return std::unexpected(SqliteError{
          .error_code = SQLITE_RANGE,
          .extended_error_code = SQLITE_RANGE,
          .message = std::string("Unknown parameter: ") + name,
          .sql = sql_,
      });
    }
    return bind(index, std::forward<T>(value));
  }

  /// Reset statement for reuse (clears bindings)
  SqliteStatus reset() {
    int result = sqlite3_reset(statement_);
    if (result != SQLITE_OK) {
      return make_error(result);
    }
    result = sqlite3_clear_bindings(statement_);
    return check_bind_result(result);
  }

  /// Clear bindings only (keeps statement ready for step)
  SqliteStatus clear_bindings() {
    int result = sqlite3_clear_bindings(statement_);
    return check_bind_result(result);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Execution
  // ─────────────────────────────────────────────────────────────────────────

  /// Execute statement that returns no rows (INSERT, UPDATE, DELETE, etc.)
  SqliteStatus execute() {
    int result = sqlite3_step(statement_);
    if (result == SQLITE_DONE) {
      return {};
    }
    if (result == SQLITE_ROW) {
      // Unexpected rows - drain them
      while (sqlite3_step(statement_) == SQLITE_ROW) {
      }
      return {};
    }
    return make_error(result);
  }

  /// Execute and return number of changes
  SqliteResult<int> execute_changes() {
    auto status = execute();
    if (!status) {
      return std::unexpected(status.error());
    }
    return sqlite3_changes(database_);
  }

  /// Step to next row
  ///
  /// @return true if a row is available, false if done
  SqliteResult<bool> step() {
    int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    return std::unexpected(make_error(result).error());
  }

  /// Get current row (only valid after step() returns true)
  [[nodiscard]] Row row() const noexcept { return Row(statement_); }

  /// Get column count
  [[nodiscard]] int column_count() const noexcept { return sqlite3_column_count(statement_); }

  // ─────────────────────────────────────────────────────────────────────────
  // Range-based iteration
  // ─────────────────────────────────────────────────────────────────────────

  /// Begin iterator - steps to first row
  [[nodiscard]] RowIterator begin() {
    // Reset to start fresh iteration
    sqlite3_reset(statement_);
    int result = sqlite3_step(statement_);
    return RowIterator(statement_, result == SQLITE_ROW);
  }

  /// End iterator
  [[nodiscard]] RowIterator end() const noexcept { return RowIterator(); }

private:
  void finalize() noexcept {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
      statement_ = nullptr;
    }
  }

  SqliteStatus check_bind_result(int result) {
    if (result != SQLITE_OK) {
      return make_error(result);
    }
    return {};
  }

  std::unexpected<SqliteError> make_error(int result) {
    return std::unexpected(SqliteError{
        .error_code = result,
        .extended_error_code = database_ ? sqlite3_extended_errcode(database_) : result,
        .message = database_ ? sqlite3_errmsg(database_) : sqlite3_errstr(result),
        .sql = sql_,
    });
  }

  sqlite3* database_ = nullptr;
  sqlite3_stmt* statement_ = nullptr;
  std::string sql_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Transaction - RAII transaction management
// ─────────────────────────────────────────────────────────────────────────────

/// Transaction type
enum class TransactionType {
  deferred,  // DEFERRED (default) - lock acquired on first access
  immediate, // IMMEDIATE - write lock acquired immediately
  exclusive, // EXCLUSIVE - exclusive lock acquired immediately
};

/// RAII transaction with automatic rollback on scope exit.
///
/// The transaction is automatically rolled back on destruction unless
/// commit() is called explicitly.
///
/// Usage:
///   auto transaction = database.begin_transaction();
///   if (transaction) {
///     // ... do work ...
///     transaction->commit();
///   }
class Transaction {
public:
  /// Construct inactive transaction
  Transaction() noexcept = default;

  /// Construct active transaction
  explicit Transaction(sqlite3* database) noexcept : database_(database), active_(true) {}

  // Non-copyable
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  // Movable
  Transaction(Transaction&& other) noexcept
      : database_(std::exchange(other.database_, nullptr)),
        active_(std::exchange(other.active_, false)) {}

  Transaction& operator=(Transaction&& other) noexcept {
    if (this != &other) {
      rollback_if_active();
      database_ = std::exchange(other.database_, nullptr);
      active_ = std::exchange(other.active_, false);
    }
    return *this;
  }

  ~Transaction() noexcept { rollback_if_active(); }

  /// Check if transaction is active
  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] explicit operator bool() const noexcept { return active_; }

  /// Commit the transaction
  SqliteStatus commit() {
    if (!active_) {
      return std::unexpected(SqliteError{
          .error_code = SQLITE_MISUSE,
          .extended_error_code = SQLITE_MISUSE,
          .message = "Transaction is not active",
          .sql = "COMMIT",
      });
    }

    char* error_message = nullptr;
    int result = sqlite3_exec(database_, "COMMIT", nullptr, nullptr, &error_message);
    active_ = false;

    if (result != SQLITE_OK) {
      std::string message = error_message ? error_message : sqlite3_errstr(result);
      if (error_message != nullptr) {
        sqlite3_free(error_message);
      }
      return std::unexpected(SqliteError{
          .error_code = result,
          .extended_error_code = sqlite3_extended_errcode(database_),
          .message = std::move(message),
          .sql = "COMMIT",
      });
    }

    return {};
  }

  /// Rollback the transaction
  SqliteStatus rollback() {
    if (!active_) {
      return std::unexpected(SqliteError{
          .error_code = SQLITE_MISUSE,
          .extended_error_code = SQLITE_MISUSE,
          .message = "Transaction is not active",
          .sql = "ROLLBACK",
      });
    }

    char* error_message = nullptr;
    int result = sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, &error_message);
    active_ = false;

    if (result != SQLITE_OK) {
      std::string message = error_message ? error_message : sqlite3_errstr(result);
      if (error_message != nullptr) {
        sqlite3_free(error_message);
      }
      return std::unexpected(SqliteError{
          .error_code = result,
          .extended_error_code = sqlite3_extended_errcode(database_),
          .message = std::move(message),
          .sql = "ROLLBACK",
      });
    }

    return {};
  }

private:
  void rollback_if_active() noexcept {
    if (active_ && database_ != nullptr) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
      active_ = false;
    }
  }

  sqlite3* database_ = nullptr;
  bool active_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Database - RAII SQLite database connection
// ─────────────────────────────────────────────────────────────────────────────

/// RAII SQLite database connection.
///
/// Opens a database connection on construction and closes it on destruction.
/// Provides methods for preparing statements, executing SQL, and managing
/// transactions.
///
/// Usage:
///   auto database = Database::open("data.db");
///   if (database) {
///     database->execute("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY)");
///     auto statement = database->prepare("INSERT INTO users VALUES (?1)");
///   }
class Database {
public:
  /// Construct closed database
  Database() noexcept = default;

  /// Construct from existing handle (takes ownership)
  explicit Database(sqlite3* handle) noexcept : database_(handle) {}

  // Non-copyable
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Movable
  Database(Database&& other) noexcept : database_(std::exchange(other.database_, nullptr)) {}

  Database& operator=(Database&& other) noexcept {
    if (this != &other) {
      close();
      database_ = std::exchange(other.database_, nullptr);
    }
    return *this;
  }

  ~Database() noexcept { close(); }

  // ─────────────────────────────────────────────────────────────────────────
  // Static factory methods
  // ─────────────────────────────────────────────────────────────────────────

  /// Open a database file
  [[nodiscard]] static SqliteResult<Database> open(const std::filesystem::path& path,
                                                   OpenMode mode = OpenMode::read_write_create) {
    int flags = 0;
    std::string uri;

    switch (mode) {
      case OpenMode::read_write_create:
        flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        break;
      case OpenMode::read_write:
        flags = SQLITE_OPEN_READWRITE;
        break;
      case OpenMode::read_only:
        flags = SQLITE_OPEN_READONLY;
        break;
      case OpenMode::immutable:
        // Use URI mode for immutable flag
        flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI;
        uri = "file:" + path.string() + "?immutable=1";
        break;
      case OpenMode::memory:
        flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY;
        break;
    }

    sqlite3* handle = nullptr;
    const char* path_string = mode == OpenMode::immutable ? uri.c_str() : path.c_str();
    int result = sqlite3_open_v2(path_string, &handle, flags, nullptr);

    if (result != SQLITE_OK) {
      std::string message = handle ? sqlite3_errmsg(handle) : sqlite3_errstr(result);
      if (handle != nullptr) {
        sqlite3_close(handle);
      }
      return std::unexpected(SqliteError{
          .error_code = result,
          .extended_error_code = result,
          .message = std::move(message),
          .sql = "OPEN " + path.string(),
      });
    }

    // Enable extended error codes
    sqlite3_extended_result_codes(handle, 1);

    return Database(handle);
  }

  /// Open an in-memory database
  [[nodiscard]] static SqliteResult<Database> open_memory() {
    return open(":memory:", OpenMode::memory);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Connection state
  // ─────────────────────────────────────────────────────────────────────────

  /// Check if database is open
  [[nodiscard]] bool is_open() const noexcept { return database_ != nullptr; }
  [[nodiscard]] explicit operator bool() const noexcept { return is_open(); }

  /// Get underlying handle
  [[nodiscard]] sqlite3* handle() const noexcept { return database_; }

  /// Close the database connection
  SqliteStatus close() noexcept {
    if (database_ == nullptr) {
      return {};
    }

    int result = sqlite3_close(database_);
    if (result != SQLITE_OK) {
      // SQLITE_BUSY means there are unfinalized statements
      return std::unexpected(SqliteError{
          .error_code = result,
          .extended_error_code = sqlite3_extended_errcode(database_),
          .message = sqlite3_errmsg(database_),
          .sql = "CLOSE",
      });
    }

    database_ = nullptr;
    return {};
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Statement preparation
  // ─────────────────────────────────────────────────────────────────────────

  /// Prepare a SQL statement
  [[nodiscard]] SqliteResult<Statement> prepare(std::string_view sql) {
    if (database_ == nullptr) {
      return std::unexpected(SqliteError{
          .error_code = SQLITE_MISUSE,
          .extended_error_code = SQLITE_MISUSE,
          .message = "Database is not open",
          .sql = std::string(sql),
      });
    }

    sqlite3_stmt* statement = nullptr;
    int result = sqlite3_prepare_v2(database_, sql.data(), static_cast<int>(sql.size()), &statement,
                                    nullptr);

    if (result != SQLITE_OK) {
      return std::unexpected(SqliteError{
          .error_code = result,
          .extended_error_code = sqlite3_extended_errcode(database_),
          .message = sqlite3_errmsg(database_),
          .sql = std::string(sql),
      });
    }

    return Statement(database_, statement, std::string(sql));
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Direct execution (for simple statements)
  // ─────────────────────────────────────────────────────────────────────────

  /// Execute a SQL statement directly (no parameters, no results)
  SqliteStatus execute(std::string_view sql) {
    if (database_ == nullptr) {
      return std::unexpected(SqliteError{
          .error_code = SQLITE_MISUSE,
          .extended_error_code = SQLITE_MISUSE,
          .message = "Database is not open",
          .sql = std::string(sql),
      });
    }

    char* error_message = nullptr;
    // Need null-terminated string for sqlite3_exec
    std::string sql_string(sql);
    int result = sqlite3_exec(database_, sql_string.c_str(), nullptr, nullptr, &error_message);

    if (result != SQLITE_OK) {
      std::string message = error_message ? error_message : sqlite3_errstr(result);
      if (error_message != nullptr) {
        sqlite3_free(error_message);
      }
      return std::unexpected(SqliteError{
          .error_code = result,
          .extended_error_code = sqlite3_extended_errcode(database_),
          .message = std::move(message),
          .sql = std::move(sql_string),
      });
    }

    return {};
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Transactions
  // ─────────────────────────────────────────────────────────────────────────

  /// Begin a transaction
  [[nodiscard]] SqliteResult<Transaction>
  begin_transaction(TransactionType type = TransactionType::deferred) {
    const char* sql = nullptr;
    switch (type) {
      case TransactionType::deferred:
        sql = "BEGIN DEFERRED";
        break;
      case TransactionType::immediate:
        sql = "BEGIN IMMEDIATE";
        break;
      case TransactionType::exclusive:
        sql = "BEGIN EXCLUSIVE";
        break;
    }

    auto status = execute(sql);
    if (!status) {
      return std::unexpected(status.error());
    }

    return Transaction(database_);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Metadata
  // ─────────────────────────────────────────────────────────────────────────

  /// Get the last inserted row ID
  [[nodiscard]] std::int64_t last_insert_rowid() const noexcept {
    return sqlite3_last_insert_rowid(database_);
  }

  /// Get the number of rows changed by the last statement
  [[nodiscard]] int changes() const noexcept { return sqlite3_changes(database_); }

  /// Get total changes since database was opened
  [[nodiscard]] int total_changes() const noexcept { return sqlite3_total_changes(database_); }

  // ─────────────────────────────────────────────────────────────────────────
  // Configuration
  // ─────────────────────────────────────────────────────────────────────────

  /// Configure database for use as a cache (disable sync, truncate journal)
  SqliteStatus configure_as_cache() {
    auto status = execute("PRAGMA synchronous = OFF");
    if (!status) {
      return status;
    }
    return execute("PRAGMA journal_mode = TRUNCATE");
  }

  /// Enable foreign key constraints
  SqliteStatus enable_foreign_keys() { return execute("PRAGMA foreign_keys = ON"); }

  /// Set busy timeout in milliseconds
  SqliteStatus set_busy_timeout(int milliseconds) {
    int result = sqlite3_busy_timeout(database_, milliseconds);
    if (result != SQLITE_OK) {
      return std::unexpected(SqliteError{
          .error_code = result,
          .extended_error_code = sqlite3_extended_errcode(database_),
          .message = sqlite3_errmsg(database_),
          .sql = "BUSY_TIMEOUT",
      });
    }
    return {};
  }

  /// Enable WAL mode
  SqliteStatus enable_wal() { return execute("PRAGMA journal_mode = WAL"); }

private:
  sqlite3* database_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// Utility functions
// ─────────────────────────────────────────────────────────────────────────────

/// Retry a database operation when busy, with exponential backoff
template <typename Func>
auto retry_on_busy(Func&& func, int max_retries = 10, int initial_delay_ms = 1)
    -> decltype(func()) {
  int delay_ms = initial_delay_ms;

  for (int attempt = 0; attempt < max_retries; ++attempt) {
    auto result = func();
    if (result) {
      return result;
    }

    if (!result.error().is_busy()) {
      return result;
    }

    // Sleep and retry with exponential backoff
    struct timespec ts;
    ts.tv_sec = delay_ms / 1000;
    ts.tv_nsec = (delay_ms % 1000) * 1000000;
    nanosleep(&ts, nullptr);

    delay_ms *= 2;
    if (delay_ms > 1000) {
      delay_ms = 1000; // Cap at 1 second
    }
  }

  // Final attempt
  return func();
}

} // namespace straylight::nix::primitives
