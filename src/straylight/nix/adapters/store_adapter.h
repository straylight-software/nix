#pragma once

/// @file store_adapter.h
/// @brief Adapter bridging straylight two-tier store to nix::store_t
///
/// This adapter implements nix::local_fs_store by delegating database
/// operations to straylight's two_tier_store while preserving the
/// filesystem operations from the nix hierarchy.
///
/// The two-tier store provides:
///   - CA tier: Content-addressed blob storage (coordination-free)
///   - Legacy tier: SQLite for input-addressed paths (flock coordination)
///
/// This adapter provides:
///   - Full nix::store_t interface compatibility
///   - Runtime switchable via component registry
///   - Backwards compatible with existing nix stores
///
/// Build operations are delegated to a nix daemon for sandboxing.

#include <memory>
#include <string>

#include "straylight/nix/store/two_tier_store.h"

#include "nix/store/indirect-root-store.h"
#include "nix/store/keys.h"
#include "nix/store/store-reference.h"
#include "nix/util/ref.h"

// Forward declare for build delegation
namespace straylight::nix::build {
class build_service;
}

namespace straylight::nix::adapters {

// ============================================================================
// Configuration
// ============================================================================

struct StoreAdapterConfig : std::enable_shared_from_this<StoreAdapterConfig>,
                            virtual ::nix::LocalFSStoreConfig {
  using LocalFSStoreConfig::LocalFSStoreConfig;

  StoreAdapterConfig(std::string_view scheme, std::string_view authority,
                     const ::nix::store_config_t::Params& params);

  static const std::string name() { return "Straylight Store"; }

  static ::nix::string_set_t uriSchemes() { return {"straylight"}; }

  static std::string doc();

  ::nix::ref<::nix::store_t> open_store() const override;

  ::nix::StoreReference getReference() const override;
};

// ============================================================================
// Store Adapter
// ============================================================================

/// Adapter that wraps straylight::nix::store::two_tier_store to implement
/// nix::IndirectRootStore.
///
/// Inherits filesystem operations from nix hierarchy, overrides database
/// operations to use our two-tier store.
class store_adapter : public virtual ::nix::IndirectRootStore {
public:
  using config_t = StoreAdapterConfig;

  ::nix::ref<const StoreAdapterConfig> config;

  store_adapter(::nix::ref<const config_t> params);
  ~store_adapter() override;

  std::string getUri();

  // --- Path validity (database operations) ---

  bool isValidPathUncached(const ::nix::store_path_t& path) override;

  ::nix::store_path_set_t queryValidPaths(const ::nix::store_path_set_t& paths,
                                          ::nix::SubstituteFlag maybeSubstitute) override;

  ::nix::store_path_set_t query_all_valid_paths() override;

  // --- Path info queries (database operations) ---

  void query_path_info_uncached(
      const ::nix::store_path_t& path,
      ::nix::Callback<std::shared_ptr<const ::nix::valid_path_info_t>> callback) noexcept override;

  void query_realisation_uncached(
      const ::nix::DrvOutput& id,
      ::nix::Callback<std::shared_ptr<const ::nix::UnkeyedRealisation>> callback) noexcept override;

  // --- References (database operations) ---

  void query_referrers(const ::nix::store_path_t& path,
                       ::nix::store_path_set_t& referrers) override;

  ::nix::store_path_set_t queryValidDerivers(const ::nix::store_path_t& path) override;

  std::map<std::string, std::optional<::nix::store_path_t>>
  queryStaticPartialDerivationOutputMap(const ::nix::store_path_t& path) override;

  std::optional<::nix::store_path_t> queryPathFromHashPart(const std::string& hash_part) override;

  // --- Registration (database operations) ---

  void add_to_store(const ::nix::valid_path_info_t& info, ::nix::source_t& source,
                    ::nix::RepairFlag repair, ::nix::CheckSigsFlag check_sigs) override;

  ::nix::store_path_t add_to_store_from_dump(::nix::source_t& dump, std::string_view name,
                                             ::nix::file_serialisation_method_t dump_method,
                                             ::nix::content_address_method_t hash_method,
                                             ::nix::hash_algorithm_t hash_algo,
                                             const ::nix::store_path_set_t& references,
                                             ::nix::RepairFlag repair) override;

  void register_drv_output(const ::nix::realisation_t& output) override;

  // --- GC roots (IndirectRootStore) ---

  void addIndirectRoot(const ::nix::Path& path) override;

  // --- Temp roots ---

  void addTempRoot(const ::nix::store_path_t& path) override;

  // --- Filesystem accessors ---

  /// Get real store dir - returns user store by default
  ::nix::Path getRealStoreDir() override;

  /// Get the actual filesystem path for a store path.
  /// Checks user store first, falls back to system store.
  ::nix::Path toRealPathForRead(const ::nix::store_path_t& path);

  ::nix::ref<::nix::source_accessor_t> getFSAccessor(bool require_valid_path) override;

  std::shared_ptr<::nix::source_accessor_t> getFSAccessor(const ::nix::store_path_t& path,
                                                          bool require_valid_path) override;

  // --- Maintenance ---

  void optimiseStore() override;

  bool verifyStore(bool check_contents, ::nix::RepairFlag repair) override;

  // --- Trust and signature verification ---

  std::optional<::nix::TrustedFlag> isTrustedClient() override;

  /// Check if path info is untrusted (lacks valid signature).
  /// Overrides base class default which always returns true.
  bool pathInfoIsUntrusted(const ::nix::valid_path_info_t& info) override;

  /// Get the set of trusted public keys for signature verification.
  const ::nix::public_keys_t& get_public_keys();

  // --- GC (GcStore) ---

  ::nix::Roots findRoots(bool censor) override;

  void collectGarbage(const ::nix::GCOptions& options, ::nix::GCResults& results) override;

  // --- Build logs (LogStore) ---

  void addBuildLog(const ::nix::store_path_t& path, std::string_view log) override;

  // --- Build operations (delegated to daemon) ---

  void build_paths(const std::vector<::nix::derived_path_t>& paths, ::nix::BuildMode build_mode,
                   std::shared_ptr<::nix::store_t> eval_store) override;

  std::vector<::nix::keyed_build_result_t>
  build_paths_with_results(const std::vector<::nix::derived_path_t>& paths,
                           ::nix::BuildMode build_mode,
                           std::shared_ptr<::nix::store_t> eval_store) override;

  ::nix::build_result_t buildDerivation(const ::nix::store_path_t& drv_path,
                                        const ::nix::basic_derivation_t& drv,
                                        ::nix::BuildMode build_mode) override;

  void ensure_path(const ::nix::store_path_t& path) override;

  // --- Direct access to underlying store ---

  [[nodiscard]] auto underlying() -> store::two_tier_store& { return *impl_; }
  [[nodiscard]] auto underlying() const -> const store::two_tier_store& { return *impl_; }

private:
  // Translation helpers
  [[nodiscard]] auto to_straylight(const ::nix::valid_path_info_t& info) const
      -> store::legacy_path_info;
  [[nodiscard]] auto from_straylight(const store::legacy_path_info& info) const
      -> std::shared_ptr<::nix::valid_path_info_t>;
  [[nodiscard]] auto to_store_path(std::string_view path) const -> ::nix::store_path_t;
  [[nodiscard]] auto to_straylight_refs(const ::nix::store_path_set_t& refs) const
      -> std::vector<std::string>;

  // Error handling
  [[noreturn]] void throw_error(store::store_tier_error err, std::string_view context) const;

  std::unique_ptr<store::two_tier_store> impl_;

  // Path to indirect GC roots
  ::nix::Path gcRootsDir;

  // Cached public keys for signature verification
  std::unique_ptr<::nix::public_keys_t> publicKeys_;

  // Build service for delegating builds (lazily initialized)
  mutable std::unique_ptr<build::build_service> build_service_;

  // System store for querying path info of system store paths (lazily initialized)
  mutable std::shared_ptr<::nix::store_t> system_store_;

  // Get or create build service
  build::build_service& get_build_service() const;

  // Get or create system store reference
  ::nix::store_t& get_system_store() const;
};

// ============================================================================
// Factory
// ============================================================================

/// Create a store adapter with the given store directory.
[[nodiscard]] auto make_store_adapter(const std::string& store_dir = "/nix/store")
    -> ::nix::ref<store_adapter>;

/// Register the store adapter with nix's store registry.
/// Call this during initialization to enable "straylight://" URIs.
void register_store_adapter();

} // namespace straylight::nix::adapters
