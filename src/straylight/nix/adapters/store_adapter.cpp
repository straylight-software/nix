// store_adapter.cpp - Implementation of straylight store to nix::store_t adapter
//
// User-space daemonless store that:
//   - Stores data in ~/.local/share/nix (writable by user)
//   - Falls back to system /nix/store for reads (shared packages)
//   - No daemon required (kernel flock + atomic rename)
//   - Delegates builds to nix daemon for sandboxing

#include "store_adapter.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "nix/store/globals.h"
#include "nix/store/make-content-addressed.h"
#include "nix/store/realisation.h"
#include "nix/store/store-registration.h"
#include "nix/store/uds-remote-store.h"
#include "nix/util/archive.h"
#include "nix/util/finally.h"
#include "nix/util/hash.h"
#include "nix/util/logging.h"
#include "nix/util/serialise.h"
#include "nix/util/source-accessor.h"

namespace straylight::nix::adapters {

namespace fs = std::filesystem;

// ============================================================================
// Helper: Get user data directory
// ============================================================================

static fs::path get_user_nix_root() {
  // Use XDG_DATA_HOME if set, otherwise ~/.local/share
  if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
    return fs::path(xdg) / "nix";
  }
  if (const char* home = std::getenv("HOME")) {
    return fs::path(home) / ".local" / "share" / "nix";
  }
  throw ::nix::Error("cannot determine user data directory: HOME not set");
}

// System store path for fallback reads
static constexpr std::string_view SYSTEM_STORE_DIR = "/nix/store";
static constexpr std::string_view SYSTEM_STATE_DIR = "/nix/var/nix";

// Check if a path exists in the system store (physical file check)
static bool path_exists_in_system_store(std::string_view store_path) {
  // store_path is like "/nix/store/abc123-foo"
  // System store is at /nix/store
  if (!store_path.starts_with(SYSTEM_STORE_DIR)) {
    return false;
  }
  return fs::exists(store_path);
}

// ============================================================================
// StoreAdapterConfig
// ============================================================================

StoreAdapterConfig::StoreAdapterConfig(std::string_view /* scheme */, std::string_view authority,
                                       const ::nix::store_config_t::Params& params)
    : ::nix::store_config_t(params), ::nix::LocalFSStoreConfig(authority, params) {
  // Set up user-space defaults if not explicitly configured
  auto user_root = get_user_nix_root();

  // real_store_dir: where store paths are actually stored (user-writable)
  if (real_store_dir.get() == store_dir || real_store_dir.get() == "/nix/store") {
    real_store_dir.assign(user_root / "store");
  }

  // stateDir: where database lives (user-writable)
  if (stateDir.get() == "/nix/var/nix" || stateDir.get().find("/nix/var") == 0) {
    stateDir.assign(user_root / "var" / "nix");
  }

  // logDir: build logs (user-writable)
  if (logDir.get() == "/nix/var/log/nix" || logDir.get().find("/nix/var") == 0) {
    logDir.assign(user_root / "var" / "log" / "nix");
  }
}

std::string StoreAdapterConfig::doc() {
  return R"(
    Straylight user-space store - daemonless, no root required.

    Stores data in ~/.local/share/nix by default:
      - ~/.local/share/nix/store   - store paths (writes)
      - ~/.local/share/nix/var     - database and state
      - System /nix/store          - fallback for reads

    Benefits:
      - No daemon required (kernel flock + atomic rename)
      - No root access needed
      - Shares packages with system store via fallback reads
      - CA paths are fully parallel (no coordination)
      - Crash-safe via WAL and atomic rename

    URI format: straylight://
  )";
}

::nix::ref<::nix::store_t> StoreAdapterConfig::open_store() const {
  return ::nix::make_ref<store_adapter>(
      ::nix::ref<const StoreAdapterConfig>(std::const_pointer_cast<const StoreAdapterConfig>(
          std::const_pointer_cast<StoreAdapterConfig>(std::dynamic_pointer_cast<StoreAdapterConfig>(
              const_cast<StoreAdapterConfig*>(this)->shared_from_this())))));
}

::nix::StoreReference StoreAdapterConfig::getReference() const {
  auto params = getQueryParams();
  return {
      .variant =
          ::nix::StoreReference::Specified{
              .scheme = *uriSchemes().begin(),
          },
      .params = std::move(params),
  };
}

// ============================================================================
// store_adapter construction
// ============================================================================

store_adapter::store_adapter(::nix::ref<const config_t> params)
    : ::nix::store_t(static_cast<const ::nix::store_t::config_t&>(*params)),
      ::nix::local_fs_store(static_cast<const ::nix::local_fs_store::config_t&>(*params)),
      config(params) {
  // Create the underlying two-tier store
  fs::path state_dir = config->stateDir.get();
  fs::create_directories(state_dir);

  impl_ = std::make_unique<store::two_tier_store>(state_dir);

  // Initialize
  auto result = impl_->init();
  if (!result) {
    throw_error(result.error(), "initializing straylight store");
  }

  // GC roots directory
  gcRootsDir = config->stateDir.get() + "/gcroots";
  fs::create_directories(gcRootsDir);
}

store_adapter::~store_adapter() = default;

std::string store_adapter::getUri() {
  return "straylight://" + config->storeDir_.get();
}

// ============================================================================
// Path validity
// ============================================================================

bool store_adapter::isValidPathUncached(const ::nix::store_path_t& path) {
  // Only check USER store for validity.
  // This ensures builds/substitutions write to user store even if
  // the path exists in system store. System store paths are still
  // readable via query_path_info and getFSAccessor fallbacks.
  return impl_->is_valid_path(printStorePath(path));
}

::nix::store_path_set_t
store_adapter::queryValidPaths(const ::nix::store_path_set_t& paths,
                               ::nix::SubstituteFlag /* maybeSubstitute */) {
  // Only return paths that are in the USER store as "valid"
  // This ensures that paths in system store get copied/migrated to user store
  // when substitution or copy_paths is invoked
  ::nix::store_path_set_t result;
  for (const auto& path : paths) {
    if (impl_->is_valid_path(printStorePath(path))) {
      result.insert(path);
    }
  }
  return result;
}

::nix::store_path_set_t store_adapter::query_all_valid_paths() {
  auto result = impl_->query_all_valid_paths();
  if (!result) {
    throw_error(result.error(), "querying all valid paths");
  }

  ::nix::store_path_set_t paths;
  for (const auto& path_str : *result) {
    paths.insert(to_store_path(path_str));
  }
  return paths;
}

// ============================================================================
// Path info queries
// ============================================================================

void store_adapter::query_path_info_uncached(
    const ::nix::store_path_t& path,
    ::nix::Callback<std::shared_ptr<const ::nix::valid_path_info_t>> callback) noexcept {
  try {
    auto path_str = printStorePath(path);

    // Try user store first
    auto result = impl_->query_path_info(path_str);

    if (result) {
      callback(from_straylight(*result));
      return;
    }

    // Not in user store, try system store fallback
    if (path_exists_in_system_store(path_str)) {
      // Path exists in system store - try to read from system store's database
      // For now, construct minimal path info from the filesystem
      // TODO: read from /nix/var/nix/db/db.sqlite for full metadata
      auto hash_sink = ::nix::hash_sink_t(::nix::hash_algorithm_t::SHA256);
      ::nix::dump_path(path_str, hash_sink);
      auto [nar_hash, nar_size] = hash_sink.finish();

      auto info = std::make_shared<::nix::valid_path_info_t>(
          path, ::nix::UnkeyedValidPathInfo{store_dir, std::move(nar_hash)});
      info->nar_size = nar_size;
      info->registrationTime = 0; // Unknown

      callback(info);
      return;
    }

    // Not found anywhere
    if (result.error() == store::store_tier_error::not_found) {
      callback(nullptr);
      return;
    }
    throw_error(result.error(), "querying path info for " + path_str);
  } catch (...) {
    callback.rethrow();
  }
}

void store_adapter::query_realisation_uncached(
    const ::nix::DrvOutput& id,
    ::nix::Callback<std::shared_ptr<const ::nix::UnkeyedRealisation>> callback) noexcept {
  try {
    // Query derivation output from our store
    // DrvOutput has drvHash and output_name
    auto drv_hash = id.strHash();
    auto output_name = id.output_name;

    auto result = impl_->query_derivation_output(drv_hash, output_name);
    if (!result) {
      callback(nullptr);
      return;
    }

    // We found the output path - build the realisation
    // UnkeyedRealisation has no default ctor, so we construct it in place
    auto realisation = std::make_shared<::nix::UnkeyedRealisation>(::nix::UnkeyedRealisation{
        .out_path = to_store_path(*result),
        .signatures = {},
        .dependentRealisations = {},
    });

    callback(realisation);
  } catch (...) {
    callback.rethrow();
  }
}

// ============================================================================
// References
// ============================================================================

void store_adapter::query_referrers(const ::nix::store_path_t& path,
                                    ::nix::store_path_set_t& referrers) {
  auto path_str = printStorePath(path);
  auto result = impl_->query_referrers(path_str);

  if (!result) {
    if (result.error() == store::store_tier_error::not_found) {
      return;
    }
    throw_error(result.error(), "querying referrers for " + path_str);
  }

  for (const auto& ref_path : *result) {
    referrers.insert(to_store_path(ref_path));
  }
}

::nix::store_path_set_t store_adapter::queryValidDerivers(const ::nix::store_path_t& /* path */) {
  // TODO: Implement derivers index in two_tier_store
  return {};
}

std::map<std::string, std::optional<::nix::store_path_t>>
store_adapter::queryStaticPartialDerivationOutputMap(const ::nix::store_path_t& path) {
  // Read from derivation file - delegate to base implementation
  return ::nix::local_fs_store::queryStaticPartialDerivationOutputMap(path);
}

std::optional<::nix::store_path_t>
store_adapter::queryPathFromHashPart(const std::string& hash_part) {
  auto all_paths = impl_->query_all_valid_paths();
  if (!all_paths) {
    return std::nullopt;
  }

  for (const auto& path : *all_paths) {
    auto base = fs::path(path).filename().string();
    if (base.starts_with(hash_part)) {
      return to_store_path(path);
    }
  }

  return std::nullopt;
}

// ============================================================================
// Registration
// ============================================================================

void store_adapter::add_to_store(const ::nix::valid_path_info_t& info, ::nix::source_t& source,
                                 ::nix::RepairFlag repair, ::nix::CheckSigsFlag /* check_sigs */) {
  // Skip only if path is already in USER store (not system store fallback)
  // This ensures packages migrate from system to user store on first use
  if (!repair && impl_->is_valid_path(printStorePath(info.path))) {
    // Consume the source to keep the protocol in sync
    source.skip(info.nar_size);
    return;
  }

  // Always write to user store (getRealStoreDir returns user store path)
  auto real_path = toRealPath(info.path);
  fs::create_directories(fs::path(real_path).parent_path());

  // Remove existing path if present (for repair or if partially written)
  if (fs::exists(real_path)) {
    fs::remove_all(real_path);
  }

  // Restore from NAR
  ::nix::restore_path(real_path, source);

  // Register in database
  auto sl_info = to_straylight(info);
  auto refs = to_straylight_refs(info.references);

  auto result = impl_->register_path(sl_info, refs);
  if (!result) {
    // Clean up on failure
    fs::remove_all(real_path);
    throw_error(result.error(), "registering path " + sl_info.path);
  }
}

::nix::store_path_t store_adapter::add_to_store_from_dump(
    ::nix::source_t& dump, std::string_view name, ::nix::file_serialisation_method_t dump_method,
    ::nix::content_address_method_t hash_method, ::nix::hash_algorithm_t hash_algo,
    const ::nix::store_path_set_t& references, ::nix::RepairFlag repair) {
  // 1. Hash the dump while reading it
  ::nix::hash_sink_t hash_sink(hash_algo);
  ::nix::tee_source_t tee_source(dump, hash_sink);

  // 2. Read entire dump into memory (simple approach)
  std::string dump_data = tee_source.drain();
  auto [dump_hash, dump_size] = hash_sink.finish();

  // 3. Compute content address and store path
  auto file_ingestion_method = hash_method.getFileIngestionMethod();
  bool methods_match =
      static_cast<::nix::file_ingestion_method_t>(dump_method) == file_ingestion_method;

  // If methods don't match, we need to restore and re-hash
  ::nix::hash_t content_hash = dump_hash;
  if (!methods_match) {
    // Create temp dir, restore, and hash the restored content
    auto temp_dir = fs::temp_directory_path() / ("nix-add-" + std::to_string(getpid()));
    fs::create_directories(temp_dir);
    auto temp_path = temp_dir / "x";

    ::nix::string_source_t source(dump_data);
    ::nix::restore_path(temp_path.string(), source, dump_method);

    content_hash = ::nix::hash_path(::nix::make_fs_source_accessor(temp_path),
                                    file_ingestion_method, hash_algo)
                       .first;

    // Move to store (done below)
    // Clean up temp on scope exit handled by finally
  }

  auto content_address_with_refs =
      ::nix::ContentAddressWithReferences::fromParts(hash_method, content_hash,
                                                     {
                                                         .others = references,
                                                         .self = false,
                                                     });

  auto dst_path = makeFixedOutputPathFromCA(name, content_address_with_refs);

  // 4. Check if already exists in USER store (not system store fallback)
  // This ensures packages migrate from system to user store
  if (!repair && impl_->is_valid_path(printStorePath(dst_path))) {
    return dst_path;
  }

  // 5. Write content to user store
  auto real_path = toRealPath(dst_path);
  fs::create_directories(fs::path(real_path).parent_path());

  if (fs::exists(real_path)) {
    fs::remove_all(real_path);
  }

  ::nix::string_source_t restore_source(dump_data);
  ::nix::restore_path(real_path, restore_source, dump_method);

  // 6. Compute NAR hash for metadata
  ::nix::hash_sink_t nar_sink(::nix::hash_algorithm_t::SHA256);
  ::nix::dump_path(real_path, nar_sink);
  auto [nar_hash, nar_size] = nar_sink.finish();

  // 7. Register in database
  auto info = ::nix::valid_path_info_t::makeFromCA(*this, name,
                                                   std::move(content_address_with_refs), nar_hash);
  info.nar_size = nar_size;

  auto sl_info = to_straylight(info);
  auto refs = to_straylight_refs(references);

  auto result = impl_->register_path(sl_info, refs);
  if (!result) {
    fs::remove_all(real_path);
    throw_error(result.error(), "registering path " + sl_info.path);
  }

  return dst_path;
}

void store_adapter::register_drv_output(const ::nix::realisation_t& /* output */) {
  // CA derivation realisations use a different schema (Realisations table)
  // than the old DerivationOutputs table. For now, we skip registration
  // since the output path is already registered via add_to_store.
  // TODO: Implement proper Realisations table support for CA derivations
}

// ============================================================================
// GC roots
// ============================================================================

void store_adapter::addIndirectRoot(const ::nix::Path& path) {
  // Add a symlink in gcRootsDir pointing to the indirect root (the user-facing symlink).
  // Important: Do NOT resolve symlinks - we want to point to the gc root itself,
  // not the store path it points to. Use absolute() instead of weakly_canonical().
  auto target = fs::absolute(path);
  auto hash = ::nix::hash_string(::nix::hash_algorithm_t::SHA256, path);
  auto linkPath = gcRootsDir + "/" + hash.to_string(::nix::hash_format_t::nix32, false);

  if (!fs::exists(linkPath)) {
    std::error_code ec;
    fs::create_symlink(target, linkPath, ec);
    // Silently ignore permission errors - we may not have write access to gcRootsDir
    if (ec && ec != std::errc::permission_denied) {
      throw std::filesystem::filesystem_error("creating indirect root symlink", target, linkPath,
                                              ec);
    }
  }
}

void store_adapter::addTempRoot(const ::nix::store_path_t& path) {
  // For now, just log
  // TODO: Implement proper temp roots with file locking
  log_debug("addTempRoot: %s", printStorePath(path));
}

// ============================================================================
// Filesystem accessors
// ============================================================================

::nix::Path store_adapter::getRealStoreDir() {
  // Return user store path for writes
  return config->real_store_dir.get();
}

::nix::Path store_adapter::toRealPathForRead(const ::nix::store_path_t& path) {
  auto path_str = path.to_string();

  // Check user store first
  auto user_path = fs::path{config->real_store_dir.get()} / path_str;
  if (fs::exists(user_path)) {
    return user_path.string();
  }

  // Fallback to system store
  auto system_path = fs::path{SYSTEM_STORE_DIR} / path_str;
  if (fs::exists(system_path)) {
    return system_path.string();
  }

  // Not found - return user store path (for future writes)
  return user_path.string();
}

::nix::ref<::nix::source_accessor_t> store_adapter::getFSAccessor(bool /* require_valid_path */) {
  // Return accessor for user store by default
  // Individual path lookups will check system store as fallback
  return ::nix::make_fs_source_accessor(std::filesystem::path{getRealStoreDir()});
}

std::shared_ptr<::nix::source_accessor_t>
store_adapter::getFSAccessor(const ::nix::store_path_t& path, bool require_valid_path) {
  auto path_str = path.to_string();

  // Check user store first
  auto user_path = fs::path{getRealStoreDir()} / path_str;
  if (fs::exists(user_path)) {
    return ::nix::make_fs_source_accessor(std::move(user_path)).get_ptr();
  }

  // Fallback to system store
  auto system_path = fs::path{SYSTEM_STORE_DIR} / path_str;
  if (fs::exists(system_path)) {
    return ::nix::make_fs_source_accessor(std::move(system_path)).get_ptr();
  }

  // Not found in either
  if (require_valid_path) {
    return nullptr;
  }

  // Return user path anyway for non-existent paths (for writes)
  return ::nix::make_fs_source_accessor(std::move(user_path)).get_ptr();
}

// ============================================================================
// Maintenance
// ============================================================================

void store_adapter::optimiseStore() {
  // TODO: Implement hardlink deduplication
  log_info("store optimisation not yet implemented for straylight store");
}

bool store_adapter::verifyStore(bool /* check_contents */, ::nix::RepairFlag /* repair */) {
  auto result = impl_->verify();
  if (!result) {
    log_error("store verification failed");
    return true; // errors remain
  }
  return !*result; // return true if errors remain
}

// ============================================================================
// Trust and signature verification
// ============================================================================

std::optional<::nix::TrustedFlag> store_adapter::isTrustedClient() {
  // Local store is always trusted
  return ::nix::Trusted;
}

const ::nix::public_keys_t& store_adapter::get_public_keys() {
  if (!publicKeys_) {
    publicKeys_ = std::make_unique<::nix::public_keys_t>(::nix::get_default_public_keys());
  }
  return *publicKeys_;
}

bool store_adapter::pathInfoIsUntrusted(const ::nix::valid_path_info_t& info) {
  // Check if signatures are required and if the info has valid signatures
  // Use global settings.requireSigs since we don't have a local config setting
  return ::nix::settings.requireSigs && !info.checkSignatures(*this, get_public_keys());
}

// ============================================================================
// Garbage collection
// ============================================================================

::nix::Roots store_adapter::findRoots(bool /* censor */) {
  ::nix::Roots roots;

  // Scan indirect roots directory
  if (fs::exists(gcRootsDir)) {
    for (const auto& entry : fs::directory_iterator(gcRootsDir)) {
      if (fs::is_symlink(entry.path())) {
        auto target = fs::read_symlink(entry.path());
        // The indirect root points to another symlink, which points to the store
        if (fs::is_symlink(target) && fs::exists(target)) {
          auto store_target = fs::read_symlink(target);
          auto store_target_str = store_target.string();
          if (isInStore(store_target_str)) {
            auto path = parseStorePath(store_target_str);
            roots[path].insert(entry.path().string());
          }
        }
      }
    }
  }

  return roots;
}

void store_adapter::collectGarbage(const ::nix::GCOptions& /* options */,
                                   ::nix::GCResults& /* results */) {
  // TODO: Implement proper garbage collection
  // For now, this is a no-op - GC requires careful coordination with
  // the two-tier store's reference tracking
  log_info("garbage collection not yet implemented for straylight store");
}

// ============================================================================
// Build logs
// ============================================================================

void store_adapter::addBuildLog(const ::nix::store_path_t& path, std::string_view log) {
  // Write build log to the log directory
  auto log_path =
      fs::path(config->logDir.get()) / ::nix::local_fs_store::drvsLogDir / printStorePath(path);
  fs::create_directories(log_path.parent_path());

  std::ofstream ofs(log_path);
  if (!ofs) {
    throw ::nix::sys_error_t("opening build log file '%s'", log_path.string());
  }
  ofs << log;
}

// ============================================================================
// Translation helpers
// ============================================================================

auto store_adapter::to_straylight(const ::nix::valid_path_info_t& info) const
    -> store::legacy_path_info {
  store::legacy_path_info sl_info;
  sl_info.path = printStorePath(info.path);
  sl_info.nar_hash = info.nar_hash.to_string(::nix::hash_format_t::base16, false);
  sl_info.registration_time = info.registrationTime;
  sl_info.nar_size = static_cast<std::int64_t>(info.nar_size);
  sl_info.ultimate = info.ultimate;

  if (info.deriver) {
    sl_info.deriver = printStorePath(*info.deriver);
  }

  for (const auto& sig : info.sigs) {
    sl_info.sigs.push_back(sig);
  }

  if (info.ca) {
    sl_info.ca = ::nix::render_content_address(*info.ca);
  }

  return sl_info;
}

auto store_adapter::from_straylight(const store::legacy_path_info& info) const
    -> std::shared_ptr<::nix::valid_path_info_t> {
  auto nar_hash =
      ::nix::hash_t::parse_any(info.nar_hash, std::optional(::nix::hash_algorithm_t::SHA256));

  auto nix_info = std::make_shared<::nix::valid_path_info_t>(to_store_path(info.path),
                                                             ::nix::UnkeyedValidPathInfo{
                                                                 store_dir,
                                                                 std::move(nar_hash),
                                                             });

  nix_info->registrationTime = info.registration_time;
  nix_info->nar_size = static_cast<uint64_t>(info.nar_size);
  nix_info->ultimate = info.ultimate;

  if (!info.deriver.empty()) {
    nix_info->deriver = to_store_path(info.deriver);
  }

  for (const auto& sig : info.sigs) {
    nix_info->sigs.insert(sig);
  }

  if (!info.ca.empty()) {
    nix_info->ca = ::nix::content_address_t::parse(info.ca);
  }

  // Query references
  auto refs_result = impl_->query_references(info.path);
  if (refs_result) {
    for (const auto& ref : *refs_result) {
      nix_info->references.insert(to_store_path(ref));
    }
  }

  return nix_info;
}

auto store_adapter::to_store_path(std::string_view path) const -> ::nix::store_path_t {
  return parseStorePath(std::string{path});
}

auto store_adapter::to_straylight_refs(const ::nix::store_path_set_t& refs) const
    -> std::vector<std::string> {
  std::vector<std::string> result;
  result.reserve(refs.size());
  for (const auto& ref : refs) {
    result.push_back(printStorePath(ref));
  }
  return result;
}

void store_adapter::throw_error(store::store_tier_error err, std::string_view context) const {
  std::string msg = std::string{context} + ": ";

  switch (err) {
    case store::store_tier_error::not_found:
      throw ::nix::InvalidPath(msg + "path not found");
    case store::store_tier_error::io_error:
      throw ::nix::sys_error_t(msg + "I/O error");
    case store::store_tier_error::database_error:
      throw ::nix::Error(msg + "database error");
    case store::store_tier_error::corrupt_data:
      throw ::nix::Error(msg + "corrupt data");
    case store::store_tier_error::already_exists:
      throw ::nix::Error(msg + "path already exists");
    case store::store_tier_error::invalid_path:
      throw ::nix::InvalidPath(msg + "invalid store path");
    case store::store_tier_error::lock_failed:
      throw ::nix::sys_error_t(msg + "failed to acquire lock");
    case store::store_tier_error::hash_mismatch:
      throw ::nix::Error(msg + "hash mismatch");
  }

  throw ::nix::Error(msg + "unknown error");
}

// ============================================================================
// Factory
// ============================================================================

auto make_store_adapter(const std::string& store_dir) -> ::nix::ref<store_adapter> {
  ::nix::store_config_t::Params params;
  params["store"] = store_dir;
  auto config = ::nix::make_ref<StoreAdapterConfig>(params);
  return ::nix::make_ref<store_adapter>(config);
}

void register_store_adapter() {
  ::nix::Implementations::add<StoreAdapterConfig>();
}

// ============================================================================
// Build operations (delegated to daemon)
// ============================================================================

::nix::store_t& store_adapter::getBuildStore() const {
  if (!buildStore_) {
    // Connect to the nix daemon for build operations
    // This provides sandboxing without requiring root for straylight store
    try {
      ::nix::store_config_t::Params params;
      auto daemon_config = ::nix::make_ref<::nix::UDSRemoteStoreConfig>(params);
      buildStore_ = daemon_config->open_store();
      log_info("connected to nix daemon for build operations");
    } catch (::nix::Error& e) {
      throw ::nix::Error("straylight store requires nix-daemon for builds: %s", e.what());
    }
  }
  return *buildStore_;
}

void store_adapter::build_paths(const std::vector<::nix::derived_path_t>& paths,
                                ::nix::BuildMode build_mode,
                                std::shared_ptr<::nix::store_t> eval_store) {
  log_debug("delegating build_paths to daemon (%d paths)", paths.size());
  auto& daemon = getBuildStore();
  daemon.build_paths(paths, build_mode, eval_store ? eval_store : buildStore_);
}

std::vector<::nix::keyed_build_result_t>
store_adapter::build_paths_with_results(const std::vector<::nix::derived_path_t>& paths,
                                        ::nix::BuildMode build_mode,
                                        std::shared_ptr<::nix::store_t> eval_store) {
  log_debug("delegating build_paths_with_results to daemon (%d paths)", paths.size());
  auto& daemon = getBuildStore();
  return daemon.build_paths_with_results(paths, build_mode, eval_store ? eval_store : buildStore_);
}

::nix::build_result_t store_adapter::buildDerivation(const ::nix::store_path_t& drv_path,
                                                     const ::nix::basic_derivation_t& drv,
                                                     ::nix::BuildMode build_mode) {
  log_debug("delegating buildDerivation to daemon: %s", printStorePath(drv_path));
  auto& daemon = getBuildStore();
  return daemon.buildDerivation(drv_path, drv, build_mode);
}

void store_adapter::ensure_path(const ::nix::store_path_t& path) {
  // First check if we have it (in user store or visible in system store)
  if (isValidPath(path)) {
    return;
  }

  // Delegate to daemon which can substitute or build
  auto& daemon = getBuildStore();
  daemon.ensure_path(path);
}

} // namespace straylight::nix::adapters

// Static registration - the straylight:// URI scheme is registered at startup
static ::nix::RegisterStoreImplementation<straylight::nix::adapters::StoreAdapterConfig>
    register_straylight_store;
