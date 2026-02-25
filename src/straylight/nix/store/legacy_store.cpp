// straylight::nix::primitives::legacy_store implementation
//
// SQLite-based store with daemonless coordination via flock.

#include "legacy_store.h"

#include <chrono>

namespace straylight::nix::store {

// ============================================================================
// SQL Schema (Nix1 compatible)
// ============================================================================

namespace {

constexpr const char* SCHEMA_SQL = R"(
-- Valid paths table (core store data)
CREATE TABLE IF NOT EXISTS ValidPaths (
    id               INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
    path             TEXT UNIQUE NOT NULL,
    hash             TEXT NOT NULL,  -- NAR hash in base16
    registrationTime INTEGER NOT NULL,
    deriver          TEXT,
    narSize          INTEGER,
    ultimate         INTEGER,  -- 1 if built locally
    sigs             TEXT,     -- space-separated signatures
    ca               TEXT      -- content-addressability assertion
);

-- References table (path dependencies)
CREATE TABLE IF NOT EXISTS Refs (
    referrer  INTEGER NOT NULL,
    reference INTEGER NOT NULL,
    PRIMARY KEY (referrer, reference),
    FOREIGN KEY (referrer) REFERENCES ValidPaths(id) ON DELETE CASCADE,
    FOREIGN KEY (reference) REFERENCES ValidPaths(id) ON DELETE RESTRICT
);

-- Derivation outputs table
CREATE TABLE IF NOT EXISTS DerivationOutputs (
    drv    INTEGER NOT NULL,
    id     TEXT NOT NULL,  -- output name (e.g., "out", "dev")
    path   TEXT NOT NULL,
    PRIMARY KEY (drv, id),
    FOREIGN KEY (drv) REFERENCES ValidPaths(id) ON DELETE CASCADE
);

-- Indexes for common queries
CREATE INDEX IF NOT EXISTS IndexReferrer ON Refs(referrer);
CREATE INDEX IF NOT EXISTS IndexReference ON Refs(reference);
CREATE INDEX IF NOT EXISTS IndexDerivationOutputs ON DerivationOutputs(path);
)";

} // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

legacy_store::legacy_store(std::filesystem::path root) : root_(std::move(root)) {}

legacy_store::~legacy_store() = default;

legacy_store::legacy_store(legacy_store&&) noexcept = default;
auto legacy_store::operator=(legacy_store&&) noexcept -> legacy_store& = default;

// ============================================================================
// Initialization
// ============================================================================

auto legacy_store::init() -> legacy_result<void> {
  namespace fs = std::filesystem;

  // Create root directory
  std::error_code ec;
  fs::create_directories(root_, ec);
  if (ec) {
    return std::unexpected(legacy_error::io_error);
  }

  // Open database
  auto result = ensure_db();
  if (!result) {
    return result;
  }

  // Create schema
  return create_schema();
}

auto legacy_store::ensure_db() -> legacy_result<void> {
  if (db_) {
    return {};
  }

  auto result = straylight::nix::compat::Database::open(
      db_path(), straylight::nix::compat::OpenMode::read_write_create);
  if (!result) {
    return std::unexpected(legacy_error::database_error);
  }

  db_ = std::make_unique<straylight::nix::compat::Database>(std::move(*result));

  // Enable WAL mode for concurrent reads
  auto wal = db_->execute("PRAGMA journal_mode = WAL");
  if (!wal) {
    return std::unexpected(legacy_error::database_error);
  }

  // Enable foreign keys
  auto fk = db_->execute("PRAGMA foreign_keys = ON");
  if (!fk) {
    return std::unexpected(legacy_error::database_error);
  }

  // Busy timeout for lock contention
  auto busy = db_->execute("PRAGMA busy_timeout = 5000");
  if (!busy) {
    return std::unexpected(legacy_error::database_error);
  }

  return {};
}

auto legacy_store::create_schema() -> legacy_result<void> {
  auto result = db_->execute(SCHEMA_SQL);
  if (!result) {
    return std::unexpected(legacy_error::database_error);
  }
  return {};
}

// ============================================================================
// Lock management
// ============================================================================

auto legacy_store::acquire_write_lock() -> legacy_result<straylight::nix::sync::exclusive_lock> {
  auto lock = straylight::nix::sync::exclusive_lock::acquire(lock_path());
  if (!lock) {
    return std::unexpected(legacy_error::lock_failed);
  }
  return std::move(*lock);
}

// ============================================================================
// Read operations
// ============================================================================

auto legacy_store::query_path_info(std::string_view store_path) -> legacy_result<legacy_path_info> {
  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  auto stmt =
      db_->prepare("SELECT path, hash, registrationTime, deriver, narSize, ultimate, sigs, ca "
                   "FROM ValidPaths WHERE path = ?");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  auto bind = stmt->bind(1, store_path);
  if (!bind) {
    return std::unexpected(legacy_error::database_error);
  }

  auto step = stmt->step();
  if (!step) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!*step) {
    return std::unexpected(legacy_error::not_found);
  }

  auto row = stmt->row();
  legacy_path_info info;
  info.path = row.column(0).get<std::string>();
  info.nar_hash = row.column(1).get<std::string>();
  info.registration_time = row.column(2).get<std::int64_t>();
  info.deriver = row.column(3).get_or<std::string>("");
  info.nar_size = row.column(4).get_or<std::int64_t>(0);
  info.ultimate = row.column(5).get_or<int>(0) != 0;
  info.sigs = deserialize_sigs(row.column(6).get_or<std::string>(""));
  info.ca = row.column(7).get_or<std::string>("");

  return info;
}

auto legacy_store::is_valid_path(std::string_view store_path) -> bool {
  auto db_result = ensure_db();
  if (!db_result) {
    return false;
  }

  auto stmt = db_->prepare("SELECT 1 FROM ValidPaths WHERE path = ?");
  if (!stmt) {
    return false;
  }

  if (!stmt->bind(1, store_path)) {
    return false;
  }

  auto step = stmt->step();
  return step && *step;
}

auto legacy_store::query_references(std::string_view store_path)
    -> legacy_result<std::vector<std::string>> {
  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  auto stmt = db_->prepare("SELECT p.path FROM Refs r "
                           "JOIN ValidPaths p ON r.reference = p.id "
                           "JOIN ValidPaths referrer ON r.referrer = referrer.id "
                           "WHERE referrer.path = ?");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!stmt->bind(1, store_path)) {
    return std::unexpected(legacy_error::database_error);
  }

  std::vector<std::string> refs;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(legacy_error::database_error);
    }
    if (!*step) {
      break;
    }
    refs.push_back(stmt->row().column(0).get<std::string>());
  }

  return refs;
}

auto legacy_store::query_referrers(std::string_view store_path)
    -> legacy_result<std::vector<std::string>> {
  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  auto stmt = db_->prepare("SELECT p.path FROM Refs r "
                           "JOIN ValidPaths p ON r.referrer = p.id "
                           "JOIN ValidPaths reference ON r.reference = reference.id "
                           "WHERE reference.path = ?");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!stmt->bind(1, store_path)) {
    return std::unexpected(legacy_error::database_error);
  }

  std::vector<std::string> referrers;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(legacy_error::database_error);
    }
    if (!*step) {
      break;
    }
    referrers.push_back(stmt->row().column(0).get<std::string>());
  }

  return referrers;
}

auto legacy_store::query_derivation_output(std::string_view drv_path, std::string_view output_name)
    -> legacy_result<std::string> {
  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  auto stmt = db_->prepare("SELECT do.path FROM DerivationOutputs do "
                           "JOIN ValidPaths vp ON do.drv = vp.id "
                           "WHERE vp.path = ? AND do.id = ?");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!stmt->bind(1, drv_path) || !stmt->bind(2, output_name)) {
    return std::unexpected(legacy_error::database_error);
  }

  auto step = stmt->step();
  if (!step) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!*step) {
    return std::unexpected(legacy_error::not_found);
  }

  return stmt->row().column(0).get<std::string>();
}

auto legacy_store::query_all_valid_paths() -> legacy_result<std::vector<std::string>> {
  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  auto stmt = db_->prepare("SELECT path FROM ValidPaths");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  std::vector<std::string> paths;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(legacy_error::database_error);
    }
    if (!*step) {
      break;
    }
    paths.push_back(stmt->row().column(0).get<std::string>());
  }

  return paths;
}

auto legacy_store::count() -> legacy_result<std::size_t> {
  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  auto stmt = db_->prepare("SELECT COUNT(*) FROM ValidPaths");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  auto step = stmt->step();
  if (!step || !*step) {
    return std::unexpected(legacy_error::database_error);
  }

  return static_cast<std::size_t>(stmt->row().column(0).get<std::int64_t>());
}

// ============================================================================
// Write operations
// ============================================================================

auto legacy_store::get_path_id(std::string_view store_path) -> legacy_result<std::int64_t> {
  auto stmt = db_->prepare("SELECT id FROM ValidPaths WHERE path = ?");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!stmt->bind(1, store_path)) {
    return std::unexpected(legacy_error::database_error);
  }

  auto step = stmt->step();
  if (!step) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!*step) {
    return std::unexpected(legacy_error::not_found);
  }

  return stmt->row().column(0).get<std::int64_t>();
}

auto legacy_store::register_path(const legacy_path_info& info,
                                 std::span<const std::string> references) -> legacy_result<void> {
  // Acquire write lock
  auto lock = acquire_write_lock();
  if (!lock) {
    return std::unexpected(lock.error());
  }

  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  // Begin transaction
  auto tx = db_->begin_transaction();
  if (!tx) {
    return std::unexpected(legacy_error::database_error);
  }

  // Insert or replace path
  auto stmt = db_->prepare("INSERT OR REPLACE INTO ValidPaths "
                           "(path, hash, registrationTime, deriver, narSize, ultimate, sigs, ca) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!stmt->bind(1, info.path) || !stmt->bind(2, info.nar_hash) ||
      !stmt->bind(3, info.registration_time) ||
      !stmt->bind(4, info.deriver.empty() ? std::optional<std::string_view>{} : info.deriver) ||
      !stmt->bind(5, info.nar_size) || !stmt->bind(6, info.ultimate ? 1 : 0) ||
      !stmt->bind(7, serialize_sigs(info.sigs)) ||
      !stmt->bind(8, info.ca.empty() ? std::optional<std::string_view>{} : info.ca)) {
    return std::unexpected(legacy_error::database_error);
  }

  auto exec = stmt->step();
  if (!exec) {
    return std::unexpected(legacy_error::database_error);
  }

  // Get the path ID
  auto path_id = get_path_id(info.path);
  if (!path_id) {
    return std::unexpected(legacy_error::database_error);
  }

  // Delete old references
  auto del = db_->prepare("DELETE FROM Refs WHERE referrer = ?");
  if (!del || !del->bind(1, *path_id) || !del->step()) {
    return std::unexpected(legacy_error::database_error);
  }

  // Insert new references
  if (!references.empty()) {
    auto ref_stmt = db_->prepare("INSERT INTO Refs (referrer, reference) "
                                 "SELECT ?, id FROM ValidPaths WHERE path = ?");
    if (!ref_stmt) {
      return std::unexpected(legacy_error::database_error);
    }

    for (const auto& ref : references) {
      ref_stmt->reset();
      if (!ref_stmt->bind(1, *path_id) || !ref_stmt->bind(2, ref)) {
        return std::unexpected(legacy_error::database_error);
      }
      auto ref_exec = ref_stmt->step();
      if (!ref_exec) {
        return std::unexpected(legacy_error::database_error);
      }
    }
  }

  // Commit transaction
  auto commit = tx->commit();
  if (!commit) {
    return std::unexpected(legacy_error::database_error);
  }

  return {};
}

auto legacy_store::invalidate_path(std::string_view store_path) -> legacy_result<void> {
  // Acquire write lock
  auto lock = acquire_write_lock();
  if (!lock) {
    return std::unexpected(lock.error());
  }

  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  auto stmt = db_->prepare("DELETE FROM ValidPaths WHERE path = ?");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!stmt->bind(1, store_path)) {
    return std::unexpected(legacy_error::database_error);
  }

  auto exec = stmt->step();
  if (!exec) {
    return std::unexpected(legacy_error::database_error);
  }

  return {};
}

auto legacy_store::add_derivation_output(std::string_view drv_path, std::string_view output_name,
                                         std::string_view output_path) -> legacy_result<void> {
  // Acquire write lock
  auto lock = acquire_write_lock();
  if (!lock) {
    return std::unexpected(lock.error());
  }

  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  // Get drv path ID
  auto drv_id = get_path_id(drv_path);
  if (!drv_id) {
    return std::unexpected(legacy_error::not_found);
  }

  auto stmt =
      db_->prepare("INSERT OR REPLACE INTO DerivationOutputs (drv, id, path) VALUES (?, ?, ?)");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  if (!stmt->bind(1, *drv_id) || !stmt->bind(2, output_name) || !stmt->bind(3, output_path)) {
    return std::unexpected(legacy_error::database_error);
  }

  auto exec = stmt->step();
  if (!exec) {
    return std::unexpected(legacy_error::database_error);
  }

  return {};
}

// ============================================================================
// Maintenance
// ============================================================================

auto legacy_store::verify() -> legacy_result<bool> {
  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  auto stmt = db_->prepare("PRAGMA integrity_check");
  if (!stmt) {
    return std::unexpected(legacy_error::database_error);
  }

  auto step = stmt->step();
  if (!step || !*step) {
    return std::unexpected(legacy_error::database_error);
  }

  auto result = stmt->row().column(0).get<std::string>();
  return result == "ok";
}

auto legacy_store::vacuum() -> legacy_result<void> {
  // Acquire write lock
  auto lock = acquire_write_lock();
  if (!lock) {
    return std::unexpected(lock.error());
  }

  auto db_result = ensure_db();
  if (!db_result) {
    return std::unexpected(db_result.error());
  }

  auto result = db_->execute("VACUUM");
  if (!result) {
    return std::unexpected(legacy_error::database_error);
  }

  return {};
}

// ============================================================================
// Signature serialization
// ============================================================================

auto legacy_store::serialize_sigs(const std::vector<std::string>& sigs) -> std::string {
  if (sigs.empty()) {
    return "";
  }

  std::string result;
  for (std::size_t i = 0; i < sigs.size(); ++i) {
    if (i > 0) {
      result += ' ';
    }
    result += sigs[i];
  }
  return result;
}

auto legacy_store::deserialize_sigs(std::string_view data) -> std::vector<std::string> {
  std::vector<std::string> sigs;
  if (data.empty()) {
    return sigs;
  }

  std::size_t start = 0;
  while (start < data.size()) {
    auto end = data.find(' ', start);
    if (end == std::string_view::npos) {
      end = data.size();
    }
    if (end > start) {
      sigs.emplace_back(data.substr(start, end - start));
    }
    start = end + 1;
  }

  return sigs;
}

} // namespace straylight::nix::store
