// straylight // nix // store // tests
//
// SQLite store operation benchmarks
//
// Benchmarks for hot-path SQLite operations in the local store.
// These are the critical paths for all local store operations.
//
// Run with: buck2 test //src/nix/store/tests:sqlite_bench

#include <array>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <sqlite3.h>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

// =============================================================================
// Test fixtures and helpers
// =============================================================================

// Schema matching the real Nix store database (schema.sql.gen.h)
constexpr const char* STORE_SCHEMA = R"sql(
  create table if not exists ValidPaths (
      id               integer primary key autoincrement not null,
      path             text unique not null,
      hash             text not null,
      registrationTime integer not null,
      deriver          text,
      narSize          integer,
      ultimate         integer,
      sigs             text,
      ca               text
  );

  create table if not exists Refs (
      referrer  integer not null,
      reference integer not null,
      primary key (referrer, reference),
      foreign key (referrer) references ValidPaths(id) on delete cascade,
      foreign key (reference) references ValidPaths(id) on delete restrict
  );

  create index if not exists IndexReferrer on Refs(referrer);
  create index if not exists IndexReference on Refs(reference);

  create trigger if not exists DeleteSelfRefs before delete on ValidPaths
    begin
      delete from Refs where referrer = old.id and reference = old.id;
    end;

  create table if not exists DerivationOutputs (
      drv  integer not null,
      id   text not null,
      path text not null,
      primary key (drv, id),
      foreign key (drv) references ValidPaths(id) on delete cascade
  );

  create index if not exists IndexDerivationOutputs on DerivationOutputs(path);

  -- GC roots simulation table (simplified from actual implementation)
  create table if not exists GcRoots (
      id   integer primary key autoincrement not null,
      path text not null,
      link text not null
  );

  create index if not exists IndexGcRootsPath on GcRoots(path);
)sql";

// Constants for hash generation
constexpr size_t HASH_LEN = 32;
constexpr size_t NAR_HASH_LEN = 64;
constexpr uint32_t HASH_RNG_SEED = 42;
constexpr uint32_t NAR_HASH_RNG_SEED = 123;
constexpr size_t SHA256_PREFIX_LEN = 7; // "sha256:"

// Generate a realistic store path hash (32 chars, base32)
std::string generate_hash() {
  static constexpr std::array<char, 32> base32 = {
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'f', 'g',
      'h', 'i', 'j', 'k', 'l', 'm', 'n', 'p', 'q', 'r', 's', 'v', 'w', 'x', 'y', 'z'};
  static std::mt19937 rng(HASH_RNG_SEED); // NOLINT: Fixed seed for reproducibility
  std::string hash;
  hash.reserve(HASH_LEN);
  for (size_t i = 0; i < HASH_LEN; ++i) {
    hash += base32.at(rng() % base32.size());
  }
  return hash;
}

// Generate a store path like /nix/store/<hash>-<name>
std::string generate_store_path(const std::string& name) {
  return "/nix/store/" + generate_hash() + "-" + name;
}

// Generate a NAR hash (sha256 in base16 with prefix)
std::string generate_nar_hash() {
  static constexpr std::array<char, 16> hex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  static std::mt19937 rng(NAR_HASH_RNG_SEED); // NOLINT: Fixed seed for reproducibility
  std::string hash = "sha256:";
  hash.reserve(SHA256_PREFIX_LEN + NAR_HASH_LEN);
  for (size_t i = 0; i < NAR_HASH_LEN; ++i) {
    hash += hex.at(rng() % hex.size());
  }
  return hash;
}

// RAII wrapper for sqlite3 prepared statement
class Stmt {
public:
  sqlite3_stmt* stmt = nullptr;
  sqlite3* db = nullptr;

  Stmt() = default;
  Stmt(sqlite3* db, const char* sql) : db(db) {
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      throw std::runtime_error(std::string("sqlite3_prepare_v2 failed: ") + sqlite3_errmsg(db));
    }
  }

  ~Stmt() {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
  }

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  Stmt(Stmt&& other) noexcept : stmt(other.stmt), db(other.db) {
    other.stmt = nullptr;
    other.db = nullptr;
  }
  Stmt& operator=(Stmt&& other) noexcept {
    if (this != &other) {
      if (stmt != nullptr) {
        sqlite3_finalize(stmt);
      }
      stmt = other.stmt;
      db = other.db;
      other.stmt = nullptr;
      other.db = nullptr;
    }
    return *this;
  }

  void reset() { sqlite3_reset(stmt); }

  void bind_text(int idx, const std::string& val) {
    sqlite3_bind_text(stmt, idx, val.c_str(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
  }

  void bind_int64(int idx, int64_t val) { sqlite3_bind_int64(stmt, idx, val); }

  void bind_null(int idx) { sqlite3_bind_null(stmt, idx); }

  bool step() {
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      return true;
    }
    if (rc == SQLITE_DONE) {
      return false;
    }
    throw std::runtime_error(std::string("sqlite3_step failed: ") + sqlite3_errmsg(db));
  }

  void exec() {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
      throw std::runtime_error(std::string("sqlite3_step (exec) failed: ") + sqlite3_errmsg(db));
    }
  }

  int64_t get_int64(int col) { return sqlite3_column_int64(stmt, col); }

  std::string get_text(int col) {
    auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text != nullptr ? std::string(text) : std::string();
  }
};

// Test fixture: in-memory SQLite database with Nix schema
class SqliteBenchFixture {
public:
  sqlite3* db = nullptr;

  Stmt queryPathInfo;
  Stmt queryReferences;
  Stmt queryReferrers;
  Stmt insertPath;
  Stmt insertRef;
  Stmt queryGcRoots;

  std::vector<std::string> paths;
  std::vector<int64_t> path_ids;

  explicit SqliteBenchFixture(size_t num_paths = 1000, size_t refs_per_path = 5) {
    // Open in-memory database
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
      throw std::runtime_error("Failed to open in-memory database");
    }

    // Enable foreign keys
    sqlite3_exec(db, "pragma foreign_keys = 1", nullptr, nullptr, nullptr);

    // Initialize schema
    char* err_msg = nullptr;
    rc = sqlite3_exec(db, STORE_SCHEMA, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
      std::string err = err_msg != nullptr ? err_msg : "unknown error";
      sqlite3_free(err_msg);
      throw std::runtime_error("Failed to create schema: " + err);
    }

    // Prepare statements (matching local-store.cpp patterns)
    queryPathInfo = Stmt(db, "select id, hash, registrationTime, deriver, narSize, "
                             "ultimate, sigs, ca from ValidPaths where path = ?;");

    queryReferences =
        Stmt(db, "select path from Refs join ValidPaths on reference = id where referrer = ?;");

    queryReferrers = Stmt(db, "select path from Refs join ValidPaths on referrer = id where "
                              "reference = (select id from ValidPaths where path = ?);");

    insertPath = Stmt(db, "insert into ValidPaths (path, hash, registrationTime, deriver, "
                          "narSize, ultimate, sigs, ca) values (?, ?, ?, ?, ?, ?, ?, ?);");

    insertRef = Stmt(db, "insert or replace into Refs (referrer, reference) values (?, ?);");

    queryGcRoots = Stmt(db, "select path, link from GcRoots;");

    // Populate test data
    populate_test_data(num_paths, refs_per_path);
  }

  ~SqliteBenchFixture() {
    if (db != nullptr) {
      sqlite3_close(db);
    }
  }

  SqliteBenchFixture(const SqliteBenchFixture&) = delete;
  SqliteBenchFixture& operator=(const SqliteBenchFixture&) = delete;
  SqliteBenchFixture(SqliteBenchFixture&&) = delete;
  SqliteBenchFixture& operator=(SqliteBenchFixture&&) = delete;

  int64_t get_last_insert_rowid() { return sqlite3_last_insert_rowid(db); }

private:
  void populate_test_data(size_t num_paths, size_t refs_per_path) {
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

    // Insert paths
    paths.reserve(num_paths);
    path_ids.reserve(num_paths);

    constexpr int64_t BASE_REG_TIME = 1700000000;
    constexpr int64_t BASE_NAR_SIZE = 10000;
    constexpr int64_t NAR_SIZE_INCREMENT = 100;

    for (size_t i = 0; i < num_paths; ++i) {
      std::string name = "package-" + std::to_string(i);
      std::string path = generate_store_path(name);
      std::string hash = generate_nar_hash();
      int64_t reg_time = BASE_REG_TIME + static_cast<int64_t>(i);
      int64_t nar_size = BASE_NAR_SIZE + static_cast<int64_t>(i) * NAR_SIZE_INCREMENT;

      insertPath.reset();
      insertPath.bind_text(1, path);
      insertPath.bind_text(2, hash);
      insertPath.bind_int64(3, reg_time);
      insertPath.bind_null(4); // deriver
      insertPath.bind_int64(5, nar_size);
      insertPath.bind_int64(6, 1); // ultimate
      insertPath.bind_null(7);     // sigs
      insertPath.bind_null(8);     // ca
      insertPath.exec();

      paths.push_back(path);
      path_ids.push_back(get_last_insert_rowid());
    }

    // Insert references (each path references some earlier paths)
    for (size_t i = refs_per_path; i < num_paths; ++i) {
      for (size_t j = 0; j < refs_per_path; ++j) {
        size_t ref_idx = i - j - 1;
        insertRef.reset();
        insertRef.bind_int64(1, path_ids[i]);
        insertRef.bind_int64(2, path_ids[ref_idx]);
        insertRef.exec();
      }
    }

    // Insert GC roots (10% of paths are roots)
    Stmt insertRoot(db, "insert into GcRoots (path, link) values (?, ?);");
    constexpr size_t GC_ROOT_INTERVAL = 10;
    constexpr size_t LINK_HASH_PREFIX_LEN = 8;
    for (size_t i = 0; i < num_paths; i += GC_ROOT_INTERVAL) {
      std::string link =
          "/nix/var/nix/gcroots/auto/" + generate_hash().substr(0, LINK_HASH_PREFIX_LEN);
      insertRoot.reset();
      insertRoot.bind_text(1, paths[i]);
      insertRoot.bind_text(2, link);
      insertRoot.exec();
    }

    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
  }
};

} // namespace

// =============================================================================
// Benchmarks
// =============================================================================

TEST_CASE("SQLite store benchmarks", "[benchmark][sqlite][store]") {
  // Create fixture with realistic data
  // 1000 paths with 5 references each is representative of a small-medium store
  constexpr size_t NUM_PATHS = 1000;
  constexpr size_t REFS_PER_PATH = 5;
  SqliteBenchFixture fixture(NUM_PATHS, REFS_PER_PATH);

  SECTION("Query single path info") {
    // Hot path: Every store operation starts with querying path info
    // This is called for every nix-build, nix-env -i, etc.
    size_t idx = 0;
    BENCHMARK("QueryPathInfo - single path") {
      fixture.queryPathInfo.reset();
      fixture.queryPathInfo.bind_text(1, fixture.paths[idx % fixture.paths.size()]);
      bool found = fixture.queryPathInfo.step();
      if (found) {
        auto id = fixture.queryPathInfo.get_int64(0);
        auto hash = fixture.queryPathInfo.get_text(1);
        (void)id;
        (void)hash;
      }
      idx++;
      return found;
    };
  }

  SECTION("Query path info batch (100 paths)") {
    // Batch query pattern used during mass substitution checks
    // and dependency resolution
    constexpr size_t BATCH_SIZE = 100;
    BENCHMARK("QueryPathInfo - batch 100") {
      int found_count = 0;
      for (size_t i = 0; i < BATCH_SIZE; ++i) {
        fixture.queryPathInfo.reset();
        fixture.queryPathInfo.bind_text(1, fixture.paths[i]);
        if (fixture.queryPathInfo.step()) {
          found_count++;
        }
      }
      return found_count;
    };
  }

  SECTION("Insert new path") {
    // Hot path during builds and downloads
    // Note: We create a unique path each time to avoid unique constraint violations
    static int insert_counter = 0;
    constexpr int64_t REG_TIME = 1700000000;
    constexpr int64_t NAR_SIZE = 12345;
    BENCHMARK("Insert new path") {
      std::string name = "bench-insert-" + std::to_string(insert_counter++);
      std::string path = generate_store_path(name);
      std::string hash = generate_nar_hash();

      fixture.insertPath.reset();
      fixture.insertPath.bind_text(1, path);
      fixture.insertPath.bind_text(2, hash);
      fixture.insertPath.bind_int64(3, REG_TIME);
      fixture.insertPath.bind_null(4); // deriver
      fixture.insertPath.bind_int64(5, NAR_SIZE);
      fixture.insertPath.bind_int64(6, 1); // ultimate
      fixture.insertPath.bind_null(7);     // sigs
      fixture.insertPath.bind_null(8);     // ca
      fixture.insertPath.exec();

      return fixture.get_last_insert_rowid();
    };
  }

  SECTION("Query references for path") {
    // Hot path: closure computation, dependency resolution
    // Called repeatedly during nix-build --dry-run, nix-store -qR, etc.
    constexpr size_t PATH_IDX = 500; // Middle of the store, has references
    BENCHMARK("QueryReferences - single path") {
      fixture.queryReferences.reset();
      fixture.queryReferences.bind_int64(1, fixture.path_ids[PATH_IDX]);
      int ref_count = 0;
      while (fixture.queryReferences.step()) {
        auto ref_path = fixture.queryReferences.get_text(0);
        (void)ref_path;
        ref_count++;
      }
      return ref_count;
    };
  }

  SECTION("Query referrers for path") {
    // Hot path: GC liveness checking, reverse dependency queries
    // Called during nix-store --gc, nix-store -q --referrers
    constexpr size_t PATH_IDX = 100; // Early path, likely has referrers
    BENCHMARK("QueryReferrers - single path") {
      fixture.queryReferrers.reset();
      fixture.queryReferrers.bind_text(1, fixture.paths[PATH_IDX]);
      int referrer_count = 0;
      while (fixture.queryReferrers.step()) {
        auto referrer_path = fixture.queryReferrers.get_text(0);
        (void)referrer_path;
        referrer_count++;
      }
      return referrer_count;
    };
  }

  SECTION("GC roots enumeration") {
    // Called at start of every GC operation
    // Must scan all roots to determine liveness
    BENCHMARK("Enumerate all GC roots") {
      fixture.queryGcRoots.reset();
      int root_count = 0;
      while (fixture.queryGcRoots.step()) {
        auto path = fixture.queryGcRoots.get_text(0);
        auto link = fixture.queryGcRoots.get_text(1);
        (void)path;
        (void)link;
        root_count++;
      }
      return root_count;
    };
  }
}

TEST_CASE("SQLite transaction overhead", "[benchmark][sqlite][store]") {
  constexpr size_t NUM_PATHS = 100;
  constexpr size_t REFS_PER_PATH = 3;
  SqliteBenchFixture fixture(NUM_PATHS, REFS_PER_PATH);

  SECTION("Transactional insert (single)") {
    static int counter = 0;
    constexpr int64_t REG_TIME = 1700000000;
    constexpr int64_t NAR_SIZE = 1000;
    BENCHMARK("Insert with transaction") {
      sqlite3_exec(fixture.db, "BEGIN", nullptr, nullptr, nullptr);
      std::string name = "txn-single-" + std::to_string(counter++);
      std::string path = generate_store_path(name);
      std::string hash = generate_nar_hash();

      fixture.insertPath.reset();
      fixture.insertPath.bind_text(1, path);
      fixture.insertPath.bind_text(2, hash);
      fixture.insertPath.bind_int64(3, REG_TIME);
      fixture.insertPath.bind_null(4);
      fixture.insertPath.bind_int64(5, NAR_SIZE);
      fixture.insertPath.bind_int64(6, 1);
      fixture.insertPath.bind_null(7);
      fixture.insertPath.bind_null(8);
      fixture.insertPath.exec();

      sqlite3_exec(fixture.db, "COMMIT", nullptr, nullptr, nullptr);
      return fixture.get_last_insert_rowid();
    };
  }

  SECTION("Batch insert (10 paths per transaction)") {
    static int batch_counter = 0;
    constexpr size_t BATCH_SIZE = 10;
    constexpr int64_t REG_TIME = 1700000000;
    constexpr int64_t NAR_SIZE = 1000;
    BENCHMARK("Batch insert 10 paths") {
      sqlite3_exec(fixture.db, "BEGIN", nullptr, nullptr, nullptr);
      int64_t last_id = 0;
      for (size_t i = 0; i < BATCH_SIZE; ++i) {
        std::string name = "batch-" + std::to_string(batch_counter++) + "-" + std::to_string(i);
        std::string path = generate_store_path(name);
        std::string hash = generate_nar_hash();

        fixture.insertPath.reset();
        fixture.insertPath.bind_text(1, path);
        fixture.insertPath.bind_text(2, hash);
        fixture.insertPath.bind_int64(3, REG_TIME);
        fixture.insertPath.bind_null(4);
        fixture.insertPath.bind_int64(5, NAR_SIZE);
        fixture.insertPath.bind_int64(6, 1);
        fixture.insertPath.bind_null(7);
        fixture.insertPath.bind_null(8);
        fixture.insertPath.exec();

        last_id = fixture.get_last_insert_rowid();
      }
      sqlite3_exec(fixture.db, "COMMIT", nullptr, nullptr, nullptr);
      return last_id;
    };
  }
}

TEST_CASE("SQLite scaling benchmarks", "[benchmark][sqlite][store][scaling]") {
  // Test with larger dataset to understand scaling behavior
  constexpr size_t NUM_PATHS = 10000;
  constexpr size_t REFS_PER_PATH = 10;
  SqliteBenchFixture large_fixture(NUM_PATHS, REFS_PER_PATH);

  SECTION("Path lookup in large store") {
    constexpr size_t PATH_IDX = 5000;
    BENCHMARK("QueryPathInfo - 10k paths store") {
      // Query path in middle of store
      large_fixture.queryPathInfo.reset();
      large_fixture.queryPathInfo.bind_text(1, large_fixture.paths[PATH_IDX]);
      return large_fixture.queryPathInfo.step();
    };
  }

  SECTION("Reference traversal in large store") {
    constexpr size_t PATH_IDX = 5000;
    BENCHMARK("QueryReferences - 10k paths store") {
      large_fixture.queryReferences.reset();
      large_fixture.queryReferences.bind_int64(1, large_fixture.path_ids[PATH_IDX]);
      int count = 0;
      while (large_fixture.queryReferences.step()) {
        count++;
      }
      return count;
    };
  }

  SECTION("Referrer lookup in large store") {
    // Path 100 should have many referrers in a 10k store with 10 refs each
    constexpr size_t PATH_IDX = 100;
    BENCHMARK("QueryReferrers - 10k paths store") {
      large_fixture.queryReferrers.reset();
      large_fixture.queryReferrers.bind_text(1, large_fixture.paths[PATH_IDX]);
      int count = 0;
      while (large_fixture.queryReferrers.step()) {
        count++;
      }
      return count;
    };
  }
}
