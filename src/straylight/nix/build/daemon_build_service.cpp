// straylight::nix::build::daemon_build_service
//
// Build service implementation that delegates to nix-daemon.
// This is the current default - provides sandboxed builds via the
// existing nix daemon infrastructure.

#include <cstdlib>

#include "build_service.h"
#include "nix/store/globals.h"
#include "nix/store/uds-remote-store.h"
#include "nix/util/logging.h"

namespace straylight::nix::build {

namespace {

// ============================================================================
// Nix Daemon Build Service Implementation
// ============================================================================

struct daemon_build_service final : build_service {
  std::string socket_path_;
  std::shared_ptr<::nix::store_t> daemon_store_;

  explicit daemon_build_service(std::string socket_path) : socket_path_(std::move(socket_path)) {}

  void build_paths(::nix::store_t& store, const std::vector<::nix::derived_path_t>& paths,
                   ::nix::BuildMode build_mode) override {
    auto& daemon = get_daemon();
    // Use the daemon as eval_store too - it can see all paths
    daemon.build_paths(paths, build_mode, daemon_store_);
  }

  auto build_paths_with_results(::nix::store_t& store,
                                const std::vector<::nix::derived_path_t>& paths,
                                ::nix::BuildMode build_mode)
      -> std::vector<::nix::keyed_build_result_t> override {
    auto& daemon = get_daemon();
    return daemon.build_paths_with_results(paths, build_mode, daemon_store_);
  }

  auto build_derivation(::nix::store_t& store, const ::nix::store_path_t& drv_path,
                        const ::nix::basic_derivation_t& drv, ::nix::BuildMode build_mode)
      -> ::nix::build_result_t override {
    auto& daemon = get_daemon();
    return daemon.buildDerivation(drv_path, drv, build_mode);
  }

  void ensure_path(::nix::store_t& store, const ::nix::store_path_t& path) override {
    // Check local store first
    if (store.isValidPath(path)) {
      return;
    }
    // Delegate to daemon
    auto& daemon = get_daemon();
    daemon.ensure_path(path);
  }

  auto name() const -> std::string_view override { return "nix-daemon"; }

  auto is_available() const -> bool override {
    // Check if socket exists
    return ::nix::path_exists(socket_path_);
  }

  auto get_daemon() -> ::nix::store_t& {
    if (!daemon_store_) {
      ::nix::store_config_t::Params params;
      if (!socket_path_.empty() && socket_path_ != ::nix::settings.nixDaemonSocketFile) {
        // Custom socket path
        auto config = std::make_shared<::nix::UDSRemoteStoreConfig>("unix", socket_path_, params);
        daemon_store_ = config->open_store();
      } else {
        // Default socket
        auto config = std::make_shared<::nix::UDSRemoteStoreConfig>(params);
        daemon_store_ = config->open_store();
      }
      log_info("build service connected to nix daemon at %s",
               socket_path_.empty() ? ::nix::settings.nixDaemonSocketFile : socket_path_);
    }
    return *daemon_store_;
  }
};

} // namespace

// ============================================================================
// Factory Functions
// ============================================================================

std::unique_ptr<build_service> make_default_build_service() {
  // Check NIX_BUILD_SERVICE environment variable
  // Values: "daemon" (default), "firecracker", "reapi"
  const char* build_service_env = getenv("NIX_BUILD_SERVICE");
  std::string_view service_type = build_service_env ? build_service_env : "";

  // Explicit firecracker request
  if (service_type == "firecracker") {
    auto svc = make_firecracker_build_service();
    if (svc && svc->is_available()) {
      log_info("using firecracker build service (explicit)");
      return svc;
    }
    log_warning("firecracker build service requested but not available, falling back to daemon");
  }

  // Explicit REAPI request
  if (service_type == "reapi") {
    const char* endpoint = getenv("NIX_REAPI_ENDPOINT");
    const char* instance = getenv("NIX_REAPI_INSTANCE");
    if (endpoint) {
      auto svc = make_reapi_build_service(endpoint, instance ? instance : "main");
      if (svc && svc->is_available()) {
        log_info("using REAPI build service at %s", endpoint);
        return svc;
      }
    }
    log_warning("REAPI build service requested but not available, falling back to daemon");
  }

  // Auto-detect: prefer firecracker if available and no daemon running
  if (service_type.empty() || service_type == "auto") {
    // Check if daemon is available
    auto daemon_svc = make_daemon_build_service();
    bool daemon_available = daemon_svc && daemon_svc->is_available();

    // If no daemon, try firecracker
    if (!daemon_available) {
      auto fc_svc = make_firecracker_build_service();
      if (fc_svc && fc_svc->is_available()) {
        log_info("using firecracker build service (auto-detected, no daemon)");
        return fc_svc;
      }
    }

    // Use daemon if available
    if (daemon_available) {
      log_info("using nix daemon build service");
      return daemon_svc;
    }
  }

  // Explicit daemon request or fallback
  return make_daemon_build_service();
}

std::unique_ptr<build_service> make_daemon_build_service() {
  return std::make_unique<daemon_build_service>("");
}

std::unique_ptr<build_service> make_daemon_build_service(const std::string& socket_path) {
  return std::make_unique<daemon_build_service>(socket_path);
}

} // namespace straylight::nix::build
