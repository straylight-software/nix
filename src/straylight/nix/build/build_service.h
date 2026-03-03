// straylight::nix::build::build_service
//
// Abstract interface for build execution backends.
// Implementations:
//   - NixDaemonBuildService: delegates to nix-daemon (current)
//   - REAPIBuildService: delegates to REAPI server (future, e.g. nativelink)
//
// This abstraction allows the straylight store to be backend-agnostic
// for build operations while maintaining storage independence.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nix/store/build-result.h"
#include "nix/store/derived-path.h"
#include "nix/store/realisation.h"
#include "nix/store/store-api.h"

namespace straylight::nix::build {

// ============================================================================
// Build Service Interface
// ============================================================================

/// Abstract interface for build execution.
/// Decouples storage from build execution, enabling different backends.
class build_service {
public:
  virtual ~build_service() = default;

  /// Build multiple paths (derivations or opaque paths via substitution)
  virtual void build_paths(::nix::store_t& store, const std::vector<::nix::derived_path_t>& paths,
                           ::nix::BuildMode build_mode) = 0;

  /// Build paths and return detailed results per path
  virtual std::vector<::nix::keyed_build_result_t>
  build_paths_with_results(::nix::store_t& store, const std::vector<::nix::derived_path_t>& paths,
                           ::nix::BuildMode build_mode) = 0;

  /// Build a single derivation (non-materialized, i.e. not from .drv file)
  virtual ::nix::build_result_t build_derivation(::nix::store_t& store,
                                                 const ::nix::store_path_t& drv_path,
                                                 const ::nix::basic_derivation_t& drv,
                                                 ::nix::BuildMode build_mode) = 0;

  /// Ensure a path exists (via substitution or building)
  virtual void ensure_path(::nix::store_t& store, const ::nix::store_path_t& path) = 0;

  /// Human-readable name for logging
  virtual std::string_view name() const = 0;

  /// Check if service is available/connected
  virtual bool is_available() const = 0;
};

// ============================================================================
// Factory
// ============================================================================

/// Create the default build service based on environment/config.
/// Currently returns NixDaemonBuildService, will support REAPI in future.
[[nodiscard]] std::unique_ptr<build_service> make_default_build_service();

/// Create a nix daemon build service explicitly
[[nodiscard]] std::unique_ptr<build_service> make_daemon_build_service();

/// Create a nix daemon build service with custom socket path
[[nodiscard]] std::unique_ptr<build_service>
make_daemon_build_service(const std::string& socket_path);

/// Create an REAPI build service (Remote Execution API)
/// @param endpoint gRPC endpoint (e.g., "localhost:8980")
/// @param instance_name REAPI instance name (default: "main")
/// @note Currently a stub - returns nullptr. Will be implemented for nativelink integration.
[[nodiscard]] std::unique_ptr<build_service>
make_reapi_build_service(const std::string& endpoint, const std::string& instance_name = "main");

} // namespace straylight::nix::build
