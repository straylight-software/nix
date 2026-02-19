#pragma once
///@file

#include <filesystem>
#include <functional>
#include <string>

#include "nix/util/error.h"

struct sqlite3;
struct sqlite3_stmt;

namespace nix {

enum class SQLiteOpenMode {
  /**
   * Open the database in read-write mode.
   * If the database does not exist, it will be created.
   */
  normal,
  /**
   * Open the database in read-write mode.
   * Fails with an error if the database does not exist.
   */
  NoCreate,
  /**
   * Open the database in immutable mode.
   * In addition to the database being read-only,
   * no wal or journal files will be created by sqlite.
   * use this mode if the database is on a read-only filesystem.
   * Fails with an error if the database does not exist.
   */
  Immutable,
};

/**
 * RAII wrapper to close a SQLite database automatically.
 */
struct SQLite {
  sqlite3* db = 0;

  SQLite() {}

  SQLite(const std::filesystem::path& path, SQLiteOpenMode mode = SQLiteOpenMode::normal);
  SQLite(const SQLite& from) = delete;
  SQLite& operator=(const SQLite& from) = delete;

  // NOTE: This is noexcept since we are only copying and assigning raw pointers.
  SQLite& operator=(SQLite&& from) noexcept {
    db = from.db;
    from.db = 0;
    return *this;
  }

  ~SQLite();

  operator sqlite3*() { return db; }

  /**
   * Disable synchronous mode, set truncate journal mode.
   */
  void isCache();

  void exec(const std::string& stmt);

  uint64_t getLastInsertedRowId();
};

/**
 * RAII wrapper to create and destroy SQLite prepared statements.
 */
struct SQLiteStmt {
  sqlite3* db = 0;
  sqlite3_stmt* stmt = 0;
  std::string sql;

  SQLiteStmt() {}

  SQLiteStmt(sqlite3* db, const std::string& sql) { create(db, sql); }

  void create(sqlite3* db, const std::string& s);
  ~SQLiteStmt();

  operator sqlite3_stmt*() { return stmt; }

  /**
   * Helper for binding / executing statements.
   */
  class use_t {
    friend struct SQLiteStmt;

  private:
    SQLiteStmt& stmt;
    unsigned int curArg = 1;
    use_t(SQLiteStmt& stmt);

  public:
    ~use_t();

    /**
     * Bind the next parameter.
     */
    use_t& operator()(std::string_view value, bool notNull = true);
    use_t& operator()(const unsigned char* data, size_t len, bool notNull = true);
    use_t& operator()(int64_t value, bool notNull = true);
    use_t& bind(); // null

    int step();

    /**
     * Execute a statement that does not return rows.
     */
    void exec();

    /**
     * For statements that return 0 or more rows. Returns true iff
     * a row is available.
     */
    bool next();

    std::string getStr(int col);
    int64_t getInt(int col);
    bool isNull(int col);
  };

  use_t use() { return use_t(*this); }
};

/**
 * RAII helper that ensures transactions are aborted unless explicitly
 * committed.
 */
struct SQLiteTxn {
  bool active = false;
  sqlite3* db;

  SQLiteTxn(sqlite3* db);

  void commit();

  ~SQLiteTxn();
};

struct SQLiteError : Error {
  std::string path;
  std::string errMsg;
  int err_no, extendedErrNo, offset;

  template <typename... Args>
  [[noreturn]] static void throw_(sqlite3* db, const std::string& fs, const Args&... args) {
    throw_(db, hint_fmt_t(fs, args...));
  }

  SQLiteError(const char* path, const char* errMsg, int err_no, int extendedErrNo, int offset,
              hint_fmt_t&& hf);

protected:
  template <typename... Args>
  SQLiteError(const char* path, const char* errMsg, int err_no, int extendedErrNo, int offset,
              const std::string& fs, const Args&... args)
      : SQLiteError(path, errMsg, err_no, extendedErrNo, offset, hint_fmt_t(fs, args...)) {}

  [[noreturn]] static void throw_(sqlite3* db, hint_fmt_t&& hf);
};

make_error(SQLiteBusy, SQLiteError);

void handle_sq_lite_busy(const SQLiteBusy& e, time_t& next_warning);

/**
 * Convenience function for retrying a SQLite transaction when the
 * database is busy.
 */
template <typename T, typename F>
T retrySQLite(F&& fun) {
  time_t next_warning = time(0) + 1;

  while (true) {
    try {
      return fun();
    } catch (SQLiteBusy& e) {
      handle_sq_lite_busy(e, next_warning);
    }
  }
}

} // namespace nix
