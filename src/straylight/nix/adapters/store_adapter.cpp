// store_adapter.cpp - Implementation of straylight store to nix::store_t adapter

#include "store_adapter.h"

#include <filesystem>
#include <fstream>

#include "nix/store/globals.h"
#include "nix/store/realisation.h"
#include "nix/store/store-registration.h"
#include "nix/util/archive.h"
#include "nix/util/finally.h"
#include "nix/util/logging.h"
#include "nix/util/source-accessor.h"

namespace straylight::nix::adapters {

namespace fs = std::filesystem;

// ============================================================================
// StoreAdapterConfig
// ============================================================================

StoreAdapterConfig::StoreAdapterConfig(std::string_view /* scheme */, std::string_view authority,
                                       const ::nix::store_config_t::Params& params)
    : ::nix::store_config_t(params), ::nix::LocalFSStoreConfig(authority, params) {}

std::string StoreAdapterConfig::doc() {
  return R"(
    Straylight two-tier store with CA and legacy tiers.

    This store replaces SQLite database operations with:
      - CA tier: Content-addressed blob storage (coordination-free)
      - Legacy tier: SQLite for input-addressed paths (flock coordination)

    Benefits:
      - No daemon required for most operations
      - CA paths are fully parallel
      - Backwards compatible with existing nix stores
      - Crash-safe via WAL and atomic rename

    URI format: straylight:///nix/store
  )";
}

::nix::ref<::nix::store_t> StoreAdapterConfig::open_store() const {
  return ::nix::make_ref<store_adapter>(
      ::nix::ref<const StoreAdapterConfig>(std::const_pointer_cast<const StoreAdapterConfig>(
          std::const_pointer_cast<StoreAdapterConfig>(std::dynamic_pointer_cast<StoreAdapterConfig>(
              const_cast<StoreAdapterConfig*>(this)->shared_from_this())))));
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
  return impl_->is_valid_path(printStorePath(path));
}

::nix::store_path_set_t
store_adapter::queryValidPaths(const ::nix::store_path_set_t& paths,
                               ::nix::SubstituteFlag /* maybeSubstitute */) {
  ::nix::store_path_set_t result;
  for (const auto& path : paths) {
    if (isValidPath(path)) {
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
    auto result = impl_->query_path_info(path_str);

    if (!result) {
      if (result.error() == store::store_tier_error::not_found) {
        callback(nullptr);
        return;
      }
      throw_error(result.error(), "querying path info for " + path_str);
    }

    callback(from_straylight(*result));
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
                                 ::nix::RepairFlag /* repair */,
                                 ::nix::CheckSigsFlag /* check_sigs */) {
  // First, write the NAR to the store
  auto real_path = toRealPath(info.path);
  fs::create_directories(fs::path(real_path).parent_path());

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
    ::nix::source_t& /* dump */, std::string_view /* name */,
    ::nix::file_serialisation_method_t /* dump_method */,
    ::nix::content_address_method_t /* hash_method */, ::nix::hash_algorithm_t /* hash_algo */,
    const ::nix::store_path_set_t& /* references */, ::nix::RepairFlag /* repair */) {
  // TODO: Proper implementation with CA support
  throw ::nix::Error("add_to_store_from_dump not yet implemented for straylight store");
}

void store_adapter::register_drv_output(const ::nix::realisation_t& output) {
  auto result = impl_->add_derivation_output(output.id.strHash(), output.id.output_name,
                                             printStorePath(output.out_path));
  if (!result) {
    throw_error(result.error(), "registering derivation output");
  }
}

// ============================================================================
// GC roots
// ============================================================================

void store_adapter::addIndirectRoot(const ::nix::Path& path) {
  // Add a symlink in gcRootsDir pointing to the indirect root
  auto target = fs::weakly_canonical(path);
  auto hash = ::nix::hash_string(::nix::hash_algorithm_t::SHA256, path);
  auto linkPath = gcRootsDir + "/" + hash.to_string(::nix::hash_format_t::nix32, false);

  if (!fs::exists(linkPath)) {
    fs::create_symlink(target, linkPath);
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

::nix::ref<::nix::source_accessor_t> store_adapter::getFSAccessor(bool /* require_valid_path */) {
  return ::nix::make_fs_source_accessor(std::filesystem::path{getRealStoreDir()});
}

std::shared_ptr<::nix::source_accessor_t>
store_adapter::getFSAccessor(const ::nix::store_path_t& path, bool require_valid_path) {
  if (require_valid_path && !isValidPath(path)) {
    return nullptr;
  }
  return getFSAccessor(require_valid_path).get_ptr();
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
// Trust
// ============================================================================

std::optional<::nix::TrustedFlag> store_adapter::isTrustedClient() {
  // Local store is always trusted
  return ::nix::Trusted;
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

} // namespace straylight::nix::adapters

// Static registration - the straylight:// URI scheme is registered at startup
static ::nix::RegisterStoreImplementation<straylight::nix::adapters::StoreAdapterConfig>
    register_straylight_store;
