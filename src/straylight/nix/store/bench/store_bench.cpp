// store_bench.cpp - Benchmark log-structured store vs SQLite
//
// Compares:
//   1. Our log-structured store (sync POSIX)
//   2. Our log-structured store (io_uring)
//   3. SQLite (current Nix implementation pattern)

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "straylight/evring/evring.h"

#include "../store.h"

namespace fs = std::filesystem;
using namespace straylight::nix::store;

// ============================================================================
// Benchmark harness
// ============================================================================

struct bench_result {
  std::string name;
  double ops_per_sec;
  double latency_us;
  std::size_t count;
};

template <typename F>
auto bench(const std::string& name, std::size_t count, F&& func) -> bench_result {
  auto start = std::chrono::high_resolution_clock::now();
  func();
  auto end = std::chrono::high_resolution_clock::now();

  auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  double duration_s = static_cast<double>(duration_ns) / 1e9;
  double ops_per_sec = static_cast<double>(count) / duration_s;
  double latency_us = (static_cast<double>(duration_ns) / 1e3) / static_cast<double>(count);

  return {name, ops_per_sec, latency_us, count};
}

void print_result(const bench_result& r) {
  std::printf("%-40s %10.0f ops/s  %8.2f μs/op  (n=%zu)\n", r.name.c_str(), r.ops_per_sec,
              r.latency_us, r.count);
}

// ============================================================================
// Test data generation
// ============================================================================

auto make_test_path_info(int idx) -> path_info {
  char hash[33];
  std::snprintf(hash, sizeof(hash), "%032x", idx);
  return path_info{
      .path = std::string("/nix/store/") + hash + "-pkg" + std::to_string(idx),
      .nar_hash = "sha256:" + std::string(hash),
      .registration_time = 1700000000 + idx,
      .deriver = "",
      .nar_size = 1024 * (1 + (idx % 100)),
      .ultimate = (idx % 2) == 0,
      .sigs = {},
      .ca = "",
  };
}

// ============================================================================
// SQLite baseline (mimics Nix's pattern)
// ============================================================================

class sqlite_store {
public:
  explicit sqlite_store(const fs::path& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
      throw std::runtime_error("Failed to open SQLite database");
    }

    // Create schema (simplified version of Nix's)
    const char* schema = R"(
      CREATE TABLE IF NOT EXISTS ValidPaths (
        id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
        path TEXT UNIQUE NOT NULL,
        hash TEXT NOT NULL,
        registrationTime INTEGER NOT NULL,
        deriver TEXT,
        narSize INTEGER,
        ultimate INTEGER,
        sigs TEXT,
        ca TEXT
      );
      CREATE INDEX IF NOT EXISTS IndexPath ON ValidPaths(path);

      CREATE TABLE IF NOT EXISTS Refs (
        referrer INTEGER NOT NULL,
        reference INTEGER NOT NULL,
        PRIMARY KEY (referrer, reference)
      );
      CREATE INDEX IF NOT EXISTS IndexReferrer ON Refs(referrer);
      CREATE INDEX IF NOT EXISTS IndexReference ON Refs(reference);
    )";

    char* err = nullptr;
    rc = sqlite3_exec(db_, schema, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
      std::string msg = err ? err : "unknown error";
      sqlite3_free(err);
      throw std::runtime_error("Failed to create schema: " + msg);
    }

    // Prepare statements
    sqlite3_prepare_v2(db_,
                       "INSERT OR REPLACE INTO ValidPaths "
                       "(path, hash, registrationTime, deriver, narSize, ultimate, sigs, ca) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                       -1, &insert_stmt_, nullptr);

    sqlite3_prepare_v2(db_, "SELECT * FROM ValidPaths WHERE path = ?", -1, &query_stmt_, nullptr);

    sqlite3_prepare_v2(db_, "SELECT 1 FROM ValidPaths WHERE path = ?", -1, &exists_stmt_, nullptr);
  }

  ~sqlite_store() {
    sqlite3_finalize(insert_stmt_);
    sqlite3_finalize(query_stmt_);
    sqlite3_finalize(exists_stmt_);
    sqlite3_close(db_);
  }

  void register_path(const path_info& info) {
    sqlite3_reset(insert_stmt_);
    sqlite3_bind_text(insert_stmt_, 1, info.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_stmt_, 2, info.nar_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_stmt_, 3, info.registration_time);
    sqlite3_bind_text(insert_stmt_, 4, info.deriver.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_stmt_, 5, info.nar_size);
    sqlite3_bind_int(insert_stmt_, 6, info.ultimate ? 1 : 0);
    sqlite3_bind_text(insert_stmt_, 7, "", -1, SQLITE_TRANSIENT); // sigs
    sqlite3_bind_text(insert_stmt_, 8, info.ca.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(insert_stmt_);
  }

  auto query_path_info(const std::string& path) -> std::optional<path_info> {
    sqlite3_reset(query_stmt_);
    sqlite3_bind_text(query_stmt_, 1, path.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(query_stmt_) == SQLITE_ROW) {
      path_info info;
      info.path = reinterpret_cast<const char*>(sqlite3_column_text(query_stmt_, 1));
      info.nar_hash = reinterpret_cast<const char*>(sqlite3_column_text(query_stmt_, 2));
      info.registration_time = sqlite3_column_int64(query_stmt_, 3);
      auto deriver = sqlite3_column_text(query_stmt_, 4);
      info.deriver = deriver ? reinterpret_cast<const char*>(deriver) : "";
      info.nar_size = sqlite3_column_int64(query_stmt_, 5);
      info.ultimate = sqlite3_column_int(query_stmt_, 6) != 0;
      auto ca = sqlite3_column_text(query_stmt_, 8);
      info.ca = ca ? reinterpret_cast<const char*>(ca) : "";
      return info;
    }
    return std::nullopt;
  }

  auto is_valid_path(const std::string& path) -> bool {
    sqlite3_reset(exists_stmt_);
    sqlite3_bind_text(exists_stmt_, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(exists_stmt_) == SQLITE_ROW;
  }

  void begin_transaction() { sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr); }

  void commit() { sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr); }

private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* insert_stmt_ = nullptr;
  sqlite3_stmt* query_stmt_ = nullptr;
  sqlite3_stmt* exists_stmt_ = nullptr;
};

// ============================================================================
// Benchmark scenarios
// ============================================================================

void run_benchmarks() {
  const std::size_t small_n = 100;
  const std::size_t medium_n = 1000;
  const std::size_t large_n = 10000;

  // Create temp directories
  auto tmp = fs::temp_directory_path() / "store_bench";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  auto log_store_path = tmp / "log_store";
  auto sqlite_path = tmp / "sqlite.db";

  std::printf("\n");
  std::printf("================================================================================\n");
  std::printf("Store Benchmark: Log-structured (POSIX/io_uring) vs SQLite\n");
  std::printf(
      "================================================================================\n\n");

  // --------------------------------------------------------------------------
  // Write benchmarks
  // --------------------------------------------------------------------------

  std::printf("--- WRITE (register_path) ---\n\n");

  // Generate test data
  std::vector<path_info> infos;
  for (std::size_t i = 0; i < large_n; ++i) {
    infos.push_back(make_test_path_info(static_cast<int>(i)));
  }

  // SQLite - individual writes
  {
    fs::remove(sqlite_path);
    sqlite_store sq(sqlite_path);
    auto r = bench("SQLite: register " + std::to_string(medium_n) + " paths (individual)", medium_n,
                   [&]() {
                     for (std::size_t i = 0; i < medium_n; ++i) {
                       sq.register_path(infos[i]);
                     }
                   });
    print_result(r);
  }

  // SQLite - batch writes
  {
    fs::remove(sqlite_path);
    sqlite_store sq(sqlite_path);
    auto r = bench("SQLite: register " + std::to_string(medium_n) + " paths (transaction)",
                   medium_n, [&]() {
                     sq.begin_transaction();
                     for (std::size_t i = 0; i < medium_n; ++i) {
                       sq.register_path(infos[i]);
                     }
                     sq.commit();
                   });
    print_result(r);
  }

  // Log store - individual writes
  {
    fs::remove_all(log_store_path);
    store s(log_store_path);
    s.init();
    auto r = bench("Log store: register " + std::to_string(small_n) + " paths (flock each)",
                   small_n, [&]() {
                     for (std::size_t i = 0; i < small_n; ++i) {
                       s.register_path(infos[i], {});
                     }
                   });
    print_result(r);
  }

  std::printf("\n");

  // --------------------------------------------------------------------------
  // Read benchmarks (need to populate first)
  // --------------------------------------------------------------------------

  std::printf("--- READ (query_path_info) ---\n\n");

  // Populate stores
  fs::remove(sqlite_path);
  sqlite_store sq_read(sqlite_path);
  sq_read.begin_transaction();
  for (std::size_t i = 0; i < large_n; ++i) {
    sq_read.register_path(infos[i]);
  }
  sq_read.commit();

  fs::remove_all(log_store_path);
  store log_read(log_store_path);
  log_read.init();
  for (std::size_t i = 0; i < large_n; ++i) {
    log_read.register_path(infos[i], {});
  }

  // Random read order
  std::vector<std::size_t> read_order(large_n);
  for (std::size_t i = 0; i < large_n; ++i) {
    read_order[i] = i;
  }
  std::mt19937 rng(42);
  std::shuffle(read_order.begin(), read_order.end(), rng);

  // SQLite reads
  {
    auto r =
        bench("SQLite: query " + std::to_string(medium_n) + " paths (random)", medium_n, [&]() {
          for (std::size_t i = 0; i < medium_n; ++i) {
            auto result = sq_read.query_path_info(infos[read_order[i]].path);
            (void)result;
          }
        });
    print_result(r);
  }

  // Log store POSIX reads
  {
    auto r = bench("Log store (POSIX): query " + std::to_string(medium_n) + " paths (random)",
                   medium_n, [&]() {
                     for (std::size_t i = 0; i < medium_n; ++i) {
                       auto result = log_read.query_path_info(infos[read_order[i]].path);
                       (void)result;
                     }
                   });
    print_result(r);
  }

  // Log store io_uring bulk reads
  {
    log_read.init_ring(256);
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < medium_n; ++i) {
      paths.push_back(infos[read_order[i]].path);
    }
    auto r = bench("Log store (io_uring): bulk_query " + std::to_string(medium_n) + " paths",
                   medium_n, [&]() {
                     auto results = log_read.bulk_query_path_info(paths);
                     (void)results;
                   });
    print_result(r);
  }

  std::printf("\n");

  // --------------------------------------------------------------------------
  // Existence check benchmarks
  // --------------------------------------------------------------------------

  std::printf("--- EXISTS (is_valid_path) ---\n\n");

  // SQLite exists
  {
    auto r = bench("SQLite: is_valid " + std::to_string(medium_n) + " paths", medium_n, [&]() {
      for (std::size_t i = 0; i < medium_n; ++i) {
        auto result = sq_read.is_valid_path(infos[read_order[i]].path);
        (void)result;
      }
    });
    print_result(r);
  }

  // Log store POSIX exists
  {
    auto r = bench("Log store (POSIX): is_valid " + std::to_string(medium_n) + " paths", medium_n,
                   [&]() {
                     for (std::size_t i = 0; i < medium_n; ++i) {
                       auto result = log_read.is_valid_path(infos[read_order[i]].path);
                       (void)result;
                     }
                   });
    print_result(r);
  }

  // Log store io_uring bulk exists
  {
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < medium_n; ++i) {
      paths.push_back(infos[read_order[i]].path);
    }
    auto r = bench("Log store (io_uring): bulk_is_valid " + std::to_string(medium_n) + " paths",
                   medium_n, [&]() {
                     auto results = log_read.bulk_is_valid_path(paths);
                     (void)results;
                   });
    print_result(r);
  }

  std::printf("\n");

  // --------------------------------------------------------------------------
  // Cleanup
  // --------------------------------------------------------------------------

  fs::remove_all(tmp);

  std::printf("================================================================================\n");
}

int main() {
  run_benchmarks();
  return 0;
}
